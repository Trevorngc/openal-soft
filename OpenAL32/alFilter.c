/**
 * OpenAL cross platform audio library
 * Copyright (C) 1999-2007 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <stdlib.h>

#include "alMain.h"
#include "alu.h"
#include "alFilter.h"
#include "alThunk.h"
#include "alError.h"


extern inline void LockFiltersRead(ALCdevice *device);
extern inline void UnlockFiltersRead(ALCdevice *device);
extern inline void LockFiltersWrite(ALCdevice *device);
extern inline void UnlockFiltersWrite(ALCdevice *device);
extern inline struct ALfilter *LookupFilter(ALCdevice *device, ALuint id);
extern inline struct ALfilter *RemoveFilter(ALCdevice *device, ALuint id);
extern inline void ALfilterState_clear(ALfilterState *filter);
extern inline void ALfilterState_copyParams(ALfilterState *restrict dst, const ALfilterState *restrict src);
extern inline void ALfilterState_processPassthru(ALfilterState *filter, const ALfloat *restrict src, ALsizei numsamples);
extern inline ALfloat calc_rcpQ_from_slope(ALfloat gain, ALfloat slope);
extern inline ALfloat calc_rcpQ_from_bandwidth(ALfloat f0norm, ALfloat bandwidth);

static void InitFilterParams(ALfilter *filter, ALenum type);


AL_API ALvoid AL_APIENTRY alGenFilters(ALsizei n, ALuint *filters)
{
    ALCdevice *device;
    ALCcontext *context;
    ALsizei cur = 0;
    ALenum err;

    context = GetContextRef();
    if(!context) return;

    if(!(n >= 0))
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);

    device = context->Device;
    for(cur = 0;cur < n;cur++)
    {
        ALfilter *filter = al_calloc(16, sizeof(ALfilter));
        if(!filter)
        {
            alDeleteFilters(cur, filters);
            SET_ERROR_AND_GOTO(context, AL_OUT_OF_MEMORY, done);
        }
        InitFilterParams(filter, AL_FILTER_NULL);

        err = NewThunkEntry(&filter->id);
        if(err == AL_NO_ERROR)
            err = InsertUIntMapEntry(&device->FilterMap, filter->id, filter);
        if(err != AL_NO_ERROR)
        {
            FreeThunkEntry(filter->id);
            memset(filter, 0, sizeof(ALfilter));
            al_free(filter);

            alDeleteFilters(cur, filters);
            SET_ERROR_AND_GOTO(context, err, done);
        }

        filters[cur] = filter->id;
    }

done:
    ALCcontext_DecRef(context);
}

AL_API ALvoid AL_APIENTRY alDeleteFilters(ALsizei n, const ALuint *filters)
{
    ALCdevice *device;
    ALCcontext *context;
    ALfilter *filter;
    ALsizei i;

    context = GetContextRef();
    if(!context) return;

    device = context->Device;
    LockFiltersWrite(device);
    if(!(n >= 0))
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);
    for(i = 0;i < n;i++)
    {
        if(filters[i] && LookupFilter(device, filters[i]) == NULL)
            SET_ERROR_AND_GOTO(context, AL_INVALID_NAME, done);
    }
    for(i = 0;i < n;i++)
    {
        if((filter=RemoveFilter(device, filters[i])) == NULL)
            continue;
        FreeThunkEntry(filter->id);

        memset(filter, 0, sizeof(*filter));
        al_free(filter);
    }

done:
    UnlockFiltersWrite(device);
    ALCcontext_DecRef(context);
}

AL_API ALboolean AL_APIENTRY alIsFilter(ALuint filter)
{
    ALCcontext *Context;
    ALboolean  result;

    Context = GetContextRef();
    if(!Context) return AL_FALSE;

    LockFiltersRead(Context->Device);
    result = ((!filter || LookupFilter(Context->Device, filter)) ?
              AL_TRUE : AL_FALSE);
    UnlockFiltersRead(Context->Device);

    ALCcontext_DecRef(Context);

    return result;
}

AL_API ALvoid AL_APIENTRY alFilteri(ALuint filter, ALenum param, ALint value)
{
    ALCcontext *Context;
    ALCdevice  *Device;
    ALfilter   *ALFilter;

    Context = GetContextRef();
    if(!Context) return;

    Device = Context->Device;
    LockFiltersWrite(Device);
    if((ALFilter=LookupFilter(Device, filter)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else
    {
        if(param == AL_FILTER_TYPE)
        {
            if(value == AL_FILTER_NULL || value == AL_FILTER_LOWPASS ||
               value == AL_FILTER_HIGHPASS || value == AL_FILTER_BANDPASS)
                InitFilterParams(ALFilter, value);
            else
                alSetError(Context, AL_INVALID_VALUE);
        }
        else
        {
            /* Call the appropriate handler */
            V(ALFilter,setParami)(Context, param, value);
        }
    }
    UnlockFiltersWrite(Device);

    ALCcontext_DecRef(Context);
}

AL_API ALvoid AL_APIENTRY alFilteriv(ALuint filter, ALenum param, const ALint *values)
{
    ALCcontext *Context;
    ALCdevice  *Device;
    ALfilter   *ALFilter;

    switch(param)
    {
        case AL_FILTER_TYPE:
            alFilteri(filter, param, values[0]);
            return;
    }

    Context = GetContextRef();
    if(!Context) return;

    Device = Context->Device;
    LockFiltersWrite(Device);
    if((ALFilter=LookupFilter(Device, filter)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else
    {
        /* Call the appropriate handler */
        V(ALFilter,setParamiv)(Context, param, values);
    }
    UnlockFiltersWrite(Device);

    ALCcontext_DecRef(Context);
}

AL_API ALvoid AL_APIENTRY alFilterf(ALuint filter, ALenum param, ALfloat value)
{
    ALCcontext *Context;
    ALCdevice  *Device;
    ALfilter   *ALFilter;

    Context = GetContextRef();
    if(!Context) return;

    Device = Context->Device;
    LockFiltersWrite(Device);
    if((ALFilter=LookupFilter(Device, filter)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else
    {
        /* Call the appropriate handler */
        V(ALFilter,setParamf)(Context, param, value);
    }
    UnlockFiltersWrite(Device);

    ALCcontext_DecRef(Context);
}

AL_API ALvoid AL_APIENTRY alFilterfv(ALuint filter, ALenum param, const ALfloat *values)
{
    ALCcontext *Context;
    ALCdevice  *Device;
    ALfilter   *ALFilter;

    Context = GetContextRef();
    if(!Context) return;

    Device = Context->Device;
    LockFiltersWrite(Device);
    if((ALFilter=LookupFilter(Device, filter)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else
    {
        /* Call the appropriate handler */
        V(ALFilter,setParamfv)(Context, param, values);
    }
    UnlockFiltersWrite(Device);

    ALCcontext_DecRef(Context);
}

AL_API ALvoid AL_APIENTRY alGetFilteri(ALuint filter, ALenum param, ALint *value)
{
    ALCcontext *Context;
    ALCdevice  *Device;
    ALfilter   *ALFilter;

    Context = GetContextRef();
    if(!Context) return;

    Device = Context->Device;
    LockFiltersRead(Device);
    if((ALFilter=LookupFilter(Device, filter)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else
    {
        if(param == AL_FILTER_TYPE)
            *value = ALFilter->type;
        else
        {
            /* Call the appropriate handler */
            V(ALFilter,getParami)(Context, param, value);
        }
    }
    UnlockFiltersRead(Device);

    ALCcontext_DecRef(Context);
}

AL_API ALvoid AL_APIENTRY alGetFilteriv(ALuint filter, ALenum param, ALint *values)
{
    ALCcontext *Context;
    ALCdevice  *Device;
    ALfilter   *ALFilter;

    switch(param)
    {
        case AL_FILTER_TYPE:
            alGetFilteri(filter, param, values);
            return;
    }

    Context = GetContextRef();
    if(!Context) return;

    Device = Context->Device;
    LockFiltersRead(Device);
    if((ALFilter=LookupFilter(Device, filter)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else
    {
        /* Call the appropriate handler */
        V(ALFilter,getParamiv)(Context, param, values);
    }
    UnlockFiltersRead(Device);

    ALCcontext_DecRef(Context);
}

AL_API ALvoid AL_APIENTRY alGetFilterf(ALuint filter, ALenum param, ALfloat *value)
{
    ALCcontext *Context;
    ALCdevice  *Device;
    ALfilter   *ALFilter;

    Context = GetContextRef();
    if(!Context) return;

    Device = Context->Device;
    LockFiltersRead(Device);
    if((ALFilter=LookupFilter(Device, filter)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else
    {
        /* Call the appropriate handler */
        V(ALFilter,getParamf)(Context, param, value);
    }
    UnlockFiltersRead(Device);

    ALCcontext_DecRef(Context);
}

AL_API ALvoid AL_APIENTRY alGetFilterfv(ALuint filter, ALenum param, ALfloat *values)
{
    ALCcontext *Context;
    ALCdevice  *Device;
    ALfilter   *ALFilter;

    Context = GetContextRef();
    if(!Context) return;

    Device = Context->Device;
    LockFiltersRead(Device);
    if((ALFilter=LookupFilter(Device, filter)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else
    {
        /* Call the appropriate handler */
        V(ALFilter,getParamfv)(Context, param, values);
    }
    UnlockFiltersRead(Device);

    ALCcontext_DecRef(Context);
}


void ALfilterState_setParams(ALfilterState *filter, ALfilterType type, ALfloat gain, ALfloat f0norm, ALfloat rcpQ)
{
    ALfloat alpha, sqrtgain_alpha_2;
    ALfloat w0, sin_w0, cos_w0;
    ALfloat a[3] = { 1.0f, 0.0f, 0.0f };
    ALfloat b[3] = { 1.0f, 0.0f, 0.0f };

    // Limit gain to -100dB
    assert(gain > 0.00001f);

    w0 = F_TAU * f0norm;
    sin_w0 = sinf(w0);
    cos_w0 = cosf(w0);
    alpha = sin_w0/2.0f * rcpQ;

    /* Calculate filter coefficients depending on filter type */
    switch(type)
    {
        case ALfilterType_HighShelf:
            sqrtgain_alpha_2 = 2.0f * sqrtf(gain) * alpha;
            b[0] =       gain*((gain+1.0f) + (gain-1.0f)*cos_w0 + sqrtgain_alpha_2);
            b[1] = -2.0f*gain*((gain-1.0f) + (gain+1.0f)*cos_w0                   );
            b[2] =       gain*((gain+1.0f) + (gain-1.0f)*cos_w0 - sqrtgain_alpha_2);
            a[0] =             (gain+1.0f) - (gain-1.0f)*cos_w0 + sqrtgain_alpha_2;
            a[1] =  2.0f*     ((gain-1.0f) - (gain+1.0f)*cos_w0                   );
            a[2] =             (gain+1.0f) - (gain-1.0f)*cos_w0 - sqrtgain_alpha_2;
            break;
        case ALfilterType_LowShelf:
            sqrtgain_alpha_2 = 2.0f * sqrtf(gain) * alpha;
            b[0] =       gain*((gain+1.0f) - (gain-1.0f)*cos_w0 + sqrtgain_alpha_2);
            b[1] =  2.0f*gain*((gain-1.0f) - (gain+1.0f)*cos_w0                   );
            b[2] =       gain*((gain+1.0f) - (gain-1.0f)*cos_w0 - sqrtgain_alpha_2);
            a[0] =             (gain+1.0f) + (gain-1.0f)*cos_w0 + sqrtgain_alpha_2;
            a[1] = -2.0f*     ((gain-1.0f) + (gain+1.0f)*cos_w0                   );
            a[2] =             (gain+1.0f) + (gain-1.0f)*cos_w0 - sqrtgain_alpha_2;
            break;
        case ALfilterType_Peaking:
            gain = sqrtf(gain);
            b[0] =  1.0f + alpha * gain;
            b[1] = -2.0f * cos_w0;
            b[2] =  1.0f - alpha * gain;
            a[0] =  1.0f + alpha / gain;
            a[1] = -2.0f * cos_w0;
            a[2] =  1.0f - alpha / gain;
            break;

        case ALfilterType_LowPass:
            b[0] = (1.0f - cos_w0) / 2.0f;
            b[1] =  1.0f - cos_w0;
            b[2] = (1.0f - cos_w0) / 2.0f;
            a[0] =  1.0f + alpha;
            a[1] = -2.0f * cos_w0;
            a[2] =  1.0f - alpha;
            break;
        case ALfilterType_HighPass:
            b[0] =  (1.0f + cos_w0) / 2.0f;
            b[1] = -(1.0f + cos_w0);
            b[2] =  (1.0f + cos_w0) / 2.0f;
            a[0] =   1.0f + alpha;
            a[1] =  -2.0f * cos_w0;
            a[2] =   1.0f - alpha;
            break;
        case ALfilterType_BandPass:
            b[0] =  alpha;
            b[1] =  0;
            b[2] = -alpha;
            a[0] =  1.0f + alpha;
            a[1] = -2.0f * cos_w0;
            a[2] =  1.0f - alpha;
            break;
    }

    filter->a1 = a[1] / a[0];
    filter->a2 = a[2] / a[0];
    filter->b0 = b[0] / a[0];
    filter->b1 = b[1] / a[0];
    filter->b2 = b[2] / a[0];
}


static void ALlowpass_setParami(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALint UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALlowpass_setParamiv(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), const ALint *UNUSED(vals))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALlowpass_setParamf(ALfilter *filter, ALCcontext *context, ALenum param, ALfloat val)
{
    switch(param)
    {
        case AL_LOWPASS_GAIN:
            if(!(val >= AL_LOWPASS_MIN_GAIN && val <= AL_LOWPASS_MAX_GAIN))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            filter->Gain = val;
            break;

        case AL_LOWPASS_GAINHF:
            if(!(val >= AL_LOWPASS_MIN_GAINHF && val <= AL_LOWPASS_MAX_GAINHF))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            filter->GainHF = val;
            break;

        default:
            SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM);
    }
}
static void ALlowpass_setParamfv(ALfilter *filter, ALCcontext *context, ALenum param, const ALfloat *vals)
{ ALlowpass_setParamf(filter, context, param, vals[0]); }

static void ALlowpass_getParami(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALint *UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALlowpass_getParamiv(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALint *UNUSED(vals))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALlowpass_getParamf(ALfilter *filter, ALCcontext *context, ALenum param, ALfloat *val)
{
    switch(param)
    {
        case AL_LOWPASS_GAIN:
            *val = filter->Gain;
            break;

        case AL_LOWPASS_GAINHF:
            *val = filter->GainHF;
            break;

        default:
            SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM);
    }
}
static void ALlowpass_getParamfv(ALfilter *filter, ALCcontext *context, ALenum param, ALfloat *vals)
{ ALlowpass_getParamf(filter, context, param, vals); }

DEFINE_ALFILTER_VTABLE(ALlowpass);


static void ALhighpass_setParami(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALint UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALhighpass_setParamiv(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), const ALint *UNUSED(vals))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALhighpass_setParamf(ALfilter *filter, ALCcontext *context, ALenum param, ALfloat val)
{
    switch(param)
    {
        case AL_HIGHPASS_GAIN:
            if(!(val >= AL_HIGHPASS_MIN_GAIN && val <= AL_HIGHPASS_MAX_GAIN))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            filter->Gain = val;
            break;

        case AL_HIGHPASS_GAINLF:
            if(!(val >= AL_HIGHPASS_MIN_GAINLF && val <= AL_HIGHPASS_MAX_GAINLF))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            filter->GainLF = val;
            break;

        default:
            SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM);
    }
}
static void ALhighpass_setParamfv(ALfilter *filter, ALCcontext *context, ALenum param, const ALfloat *vals)
{ ALhighpass_setParamf(filter, context, param, vals[0]); }

static void ALhighpass_getParami(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALint *UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALhighpass_getParamiv(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALint *UNUSED(vals))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALhighpass_getParamf(ALfilter *filter, ALCcontext *context, ALenum param, ALfloat *val)
{
    switch(param)
    {
        case AL_HIGHPASS_GAIN:
            *val = filter->Gain;
            break;

        case AL_HIGHPASS_GAINLF:
            *val = filter->GainLF;
            break;

        default:
            SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM);
    }
}
static void ALhighpass_getParamfv(ALfilter *filter, ALCcontext *context, ALenum param, ALfloat *vals)
{ ALhighpass_getParamf(filter, context, param, vals); }

DEFINE_ALFILTER_VTABLE(ALhighpass);


static void ALbandpass_setParami(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALint UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALbandpass_setParamiv(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), const ALint *UNUSED(vals))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALbandpass_setParamf(ALfilter *filter, ALCcontext *context, ALenum param, ALfloat val)
{
    switch(param)
    {
        case AL_BANDPASS_GAIN:
            if(!(val >= AL_BANDPASS_MIN_GAIN && val <= AL_BANDPASS_MAX_GAIN))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            filter->Gain = val;
            break;

        case AL_BANDPASS_GAINHF:
            if(!(val >= AL_BANDPASS_MIN_GAINHF && val <= AL_BANDPASS_MAX_GAINHF))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            filter->GainHF = val;
            break;

        case AL_BANDPASS_GAINLF:
            if(!(val >= AL_BANDPASS_MIN_GAINLF && val <= AL_BANDPASS_MAX_GAINLF))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            filter->GainLF = val;
            break;

        default:
            SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM);
    }
}
static void ALbandpass_setParamfv(ALfilter *filter, ALCcontext *context, ALenum param, const ALfloat *vals)
{ ALbandpass_setParamf(filter, context, param, vals[0]); }

static void ALbandpass_getParami(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALint *UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALbandpass_getParamiv(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALint *UNUSED(vals))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALbandpass_getParamf(ALfilter *filter, ALCcontext *context, ALenum param, ALfloat *val)
{
    switch(param)
    {
        case AL_BANDPASS_GAIN:
            *val = filter->Gain;
            break;

        case AL_BANDPASS_GAINHF:
            *val = filter->GainHF;
            break;

        case AL_BANDPASS_GAINLF:
            *val = filter->GainLF;
            break;

        default:
            SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM);
    }
}
static void ALbandpass_getParamfv(ALfilter *filter, ALCcontext *context, ALenum param, ALfloat *vals)
{ ALbandpass_getParamf(filter, context, param, vals); }

DEFINE_ALFILTER_VTABLE(ALbandpass);


static void ALnullfilter_setParami(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALint UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALnullfilter_setParamiv(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), const ALint *UNUSED(vals))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALnullfilter_setParamf(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALfloat UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALnullfilter_setParamfv(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), const ALfloat *UNUSED(vals))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }

static void ALnullfilter_getParami(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALint *UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALnullfilter_getParamiv(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALint *UNUSED(vals))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALnullfilter_getParamf(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALfloat *UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
static void ALnullfilter_getParamfv(ALfilter *UNUSED(filter), ALCcontext *context, ALenum UNUSED(param), ALfloat *UNUSED(vals))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }

DEFINE_ALFILTER_VTABLE(ALnullfilter);


ALvoid ReleaseALFilters(ALCdevice *device)
{
    ALsizei i;
    for(i = 0;i < device->FilterMap.size;i++)
    {
        ALfilter *temp = device->FilterMap.values[i];
        device->FilterMap.values[i] = NULL;

        // Release filter structure
        FreeThunkEntry(temp->id);
        memset(temp, 0, sizeof(ALfilter));
        al_free(temp);
    }
}


static void InitFilterParams(ALfilter *filter, ALenum type)
{
    if(type == AL_FILTER_LOWPASS)
    {
        filter->Gain = AL_LOWPASS_DEFAULT_GAIN;
        filter->GainHF = AL_LOWPASS_DEFAULT_GAINHF;
        filter->HFReference = LOWPASSFREQREF;
        filter->GainLF = 1.0f;
        filter->LFReference = HIGHPASSFREQREF;
        filter->vtbl = &ALlowpass_vtable;
    }
    else if(type == AL_FILTER_HIGHPASS)
    {
        filter->Gain = AL_HIGHPASS_DEFAULT_GAIN;
        filter->GainHF = 1.0f;
        filter->HFReference = LOWPASSFREQREF;
        filter->GainLF = AL_HIGHPASS_DEFAULT_GAINLF;
        filter->LFReference = HIGHPASSFREQREF;
        filter->vtbl = &ALhighpass_vtable;
    }
    else if(type == AL_FILTER_BANDPASS)
    {
        filter->Gain = AL_BANDPASS_DEFAULT_GAIN;
        filter->GainHF = AL_BANDPASS_DEFAULT_GAINHF;
        filter->HFReference = LOWPASSFREQREF;
        filter->GainLF = AL_BANDPASS_DEFAULT_GAINLF;
        filter->LFReference = HIGHPASSFREQREF;
        filter->vtbl = &ALbandpass_vtable;
    }
    else
    {
        filter->Gain = 1.0f;
        filter->GainHF = 1.0f;
        filter->HFReference = LOWPASSFREQREF;
        filter->GainLF = 1.0f;
        filter->LFReference = HIGHPASSFREQREF;
        filter->vtbl = &ALnullfilter_vtable;
    }
    filter->type = type;
}
