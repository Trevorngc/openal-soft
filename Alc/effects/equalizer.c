/**
 * OpenAL cross platform audio library
 * Copyright (C) 2013 by Mike Gorchak
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

#include <math.h>
#include <stdlib.h>

#include "alMain.h"
#include "alFilter.h"
#include "alAuxEffectSlot.h"
#include "alError.h"
#include "alu.h"


/*  The document  "Effects Extension Guide.pdf"  says that low and high  *
 *  frequencies are cutoff frequencies. This is not fully correct, they  *
 *  are corner frequencies for low and high shelf filters. If they were  *
 *  just cutoff frequencies, there would be no need in cutoff frequency  *
 *  gains, which are present.  Documentation for  "Creative Proteus X2"  *
 *  software describes  4-band equalizer functionality in a much better  *
 *  way.  This equalizer seems  to be a predecessor  of  OpenAL  4-band  *
 *  equalizer.  With low and high  shelf filters  we are able to cutoff  *
 *  frequencies below and/or above corner frequencies using attenuation  *
 *  gains (below 1.0) and amplify all low and/or high frequencies using  *
 *  gains above 1.0.                                                     *
 *                                                                       *
 *     Low-shelf       Low Mid Band      High Mid Band     High-shelf    *
 *      corner            center             center          corner      *
 *     frequency        frequency          frequency       frequency     *
 *    50Hz..800Hz     200Hz..3000Hz      1000Hz..8000Hz  4000Hz..16000Hz *
 *                                                                       *
 *          |               |                  |               |         *
 *          |               |                  |               |         *
 *   B -----+            /--+--\            /--+--\            +-----    *
 *   O      |\          |   |   |          |   |   |          /|         *
 *   O      | \        -    |    -        -    |    -        / |         *
 *   S +    |  \      |     |     |      |     |     |      /  |         *
 *   T      |   |    |      |      |    |      |      |    |   |         *
 * ---------+---------------+------------------+---------------+-------- *
 *   C      |   |    |      |      |    |      |      |    |   |         *
 *   U -    |  /      |     |     |      |     |     |      \  |         *
 *   T      | /        -    |    -        -    |    -        \ |         *
 *   O      |/          |   |   |          |   |   |          \|         *
 *   F -----+            \--+--/            \--+--/            +-----    *
 *   F      |               |                  |               |         *
 *          |               |                  |               |         *
 *                                                                       *
 * Gains vary from 0.126 up to 7.943, which means from -18dB attenuation *
 * up to +18dB amplification. Band width varies from 0.01 up to 1.0 in   *
 * octaves for two mid bands.                                            *
 *                                                                       *
 * Implementation is based on the "Cookbook formulae for audio EQ biquad *
 * filter coefficients" by Robert Bristow-Johnson                        *
 * http://www.musicdsp.org/files/Audio-EQ-Cookbook.txt                   */


typedef struct ALequalizerState {
    DERIVE_FROM_TYPE(ALeffectState);

    struct {
        /* Effect gains for each channel */
        ALfloat CurrentGains[MAX_OUTPUT_CHANNELS];
        ALfloat TargetGains[MAX_OUTPUT_CHANNELS];

        /* Effect parameters */
        ALfilterState filter[4];
    } Chans[MAX_EFFECT_CHANNELS];

    ALfloat SampleBuffer[MAX_EFFECT_CHANNELS][BUFFERSIZE];
} ALequalizerState;

static ALvoid ALequalizerState_Destruct(ALequalizerState *state);
static ALboolean ALequalizerState_deviceUpdate(ALequalizerState *state, ALCdevice *device);
static ALvoid ALequalizerState_update(ALequalizerState *state, const ALCcontext *context, const ALeffectslot *slot, const ALeffectProps *props);
static ALvoid ALequalizerState_process(ALequalizerState *state, ALsizei SamplesToDo, const ALfloat (*restrict SamplesIn)[BUFFERSIZE], ALfloat (*restrict SamplesOut)[BUFFERSIZE], ALsizei NumChannels);
DECLARE_DEFAULT_ALLOCATORS(ALequalizerState)

DEFINE_ALEFFECTSTATE_VTABLE(ALequalizerState);


static void ALequalizerState_Construct(ALequalizerState *state)
{
    ALeffectState_Construct(STATIC_CAST(ALeffectState, state));
    SET_VTABLE2(ALequalizerState, ALeffectState, state);
}

static ALvoid ALequalizerState_Destruct(ALequalizerState *state)
{
    ALeffectState_Destruct(STATIC_CAST(ALeffectState,state));
}

static ALboolean ALequalizerState_deviceUpdate(ALequalizerState *state, ALCdevice *UNUSED(device))
{
    ALsizei i, j;

    for(i = 0; i < MAX_EFFECT_CHANNELS;i++)
    {
        for(j = 0;j < 4;j++)
            ALfilterState_clear(&state->Chans[i].filter[j]);
        for(j = 0;j < MAX_OUTPUT_CHANNELS;j++)
            state->Chans[i].CurrentGains[j] = 0.0f;
    }
    return AL_TRUE;
}

static ALvoid ALequalizerState_update(ALequalizerState *state, const ALCcontext *context, const ALeffectslot *slot, const ALeffectProps *props)
{
    const ALCdevice *device = context->Device;
    ALfloat frequency = (ALfloat)device->Frequency;
    ALfloat gain, f0norm;
    ALuint i;

    STATIC_CAST(ALeffectState,state)->OutBuffer = device->FOAOut.Buffer;
    STATIC_CAST(ALeffectState,state)->OutChannels = device->FOAOut.NumChannels;
    for(i = 0;i < MAX_EFFECT_CHANNELS;i++)
        ComputeFirstOrderGains(&device->FOAOut, IdentityMatrixf.m[i],
                               slot->Params.Gain, state->Chans[i].TargetGains);

    /* Calculate coefficients for the each type of filter. Note that the shelf
     * filters' gain is for the reference frequency, which is the centerpoint
     * of the transition band.
     */
    gain = maxf(sqrtf(props->Equalizer.LowGain), 0.0625f); /* Limit -24dB */
    f0norm = props->Equalizer.LowCutoff/frequency;
    ALfilterState_setParams(&state->Chans[0].filter[0], ALfilterType_LowShelf,
        gain, f0norm, calc_rcpQ_from_slope(gain, 0.75f)
    );

    gain = maxf(props->Equalizer.Mid1Gain, 0.0625f);
    f0norm = props->Equalizer.Mid1Center/frequency;
    ALfilterState_setParams(&state->Chans[0].filter[1], ALfilterType_Peaking,
        gain, f0norm, calc_rcpQ_from_bandwidth(
            f0norm, props->Equalizer.Mid1Width
        )
    );

    gain = maxf(props->Equalizer.Mid2Gain, 0.0625f);
    f0norm = props->Equalizer.Mid2Center/frequency;
    ALfilterState_setParams(&state->Chans[0].filter[2], ALfilterType_Peaking,
        gain, f0norm, calc_rcpQ_from_bandwidth(
            f0norm, props->Equalizer.Mid2Width
        )
    );

    gain = maxf(sqrtf(props->Equalizer.HighGain), 0.0625f);
    f0norm = props->Equalizer.HighCutoff/frequency;
    ALfilterState_setParams(&state->Chans[0].filter[3], ALfilterType_HighShelf,
        gain, f0norm, calc_rcpQ_from_slope(gain, 0.75f)
    );

    /* Copy the filter coefficients for the other input channels. */
    for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
    {
        ALfilterState_copyParams(&state->Chans[i].filter[0], &state->Chans[0].filter[0]);
        ALfilterState_copyParams(&state->Chans[i].filter[1], &state->Chans[0].filter[1]);
        ALfilterState_copyParams(&state->Chans[i].filter[2], &state->Chans[0].filter[2]);
        ALfilterState_copyParams(&state->Chans[i].filter[3], &state->Chans[0].filter[3]);
    }
}

static ALvoid ALequalizerState_process(ALequalizerState *state, ALsizei SamplesToDo, const ALfloat (*restrict SamplesIn)[BUFFERSIZE], ALfloat (*restrict SamplesOut)[BUFFERSIZE], ALsizei NumChannels)
{
    ALfloat (*restrict temps)[BUFFERSIZE] = state->SampleBuffer;
    ALsizei c;

    for(c = 0;c < MAX_EFFECT_CHANNELS;c++)
    {
        ALfilterState_process(&state->Chans[c].filter[0], temps[0], SamplesIn[c], SamplesToDo);
        ALfilterState_process(&state->Chans[c].filter[1], temps[1], temps[0], SamplesToDo);
        ALfilterState_process(&state->Chans[c].filter[2], temps[2], temps[1], SamplesToDo);
        ALfilterState_process(&state->Chans[c].filter[3], temps[3], temps[2], SamplesToDo);

        MixSamples(temps[3], NumChannels, SamplesOut,
            state->Chans[c].CurrentGains, state->Chans[c].TargetGains,
            SamplesToDo, 0, SamplesToDo
        );
    }
}


typedef struct ALequalizerStateFactory {
    DERIVE_FROM_TYPE(ALeffectStateFactory);
} ALequalizerStateFactory;

ALeffectState *ALequalizerStateFactory_create(ALequalizerStateFactory *UNUSED(factory))
{
    ALequalizerState *state;

    NEW_OBJ0(state, ALequalizerState)();
    if(!state) return NULL;

    return STATIC_CAST(ALeffectState, state);
}

DEFINE_ALEFFECTSTATEFACTORY_VTABLE(ALequalizerStateFactory);

ALeffectStateFactory *ALequalizerStateFactory_getFactory(void)
{
    static ALequalizerStateFactory EqualizerFactory = { { GET_VTABLE2(ALequalizerStateFactory, ALeffectStateFactory) } };

    return STATIC_CAST(ALeffectStateFactory, &EqualizerFactory);
}


void ALequalizer_setParami(ALeffect *UNUSED(effect), ALCcontext *context, ALenum UNUSED(param), ALint UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
void ALequalizer_setParamiv(ALeffect *effect, ALCcontext *context, ALenum param, const ALint *vals)
{
    ALequalizer_setParami(effect, context, param, vals[0]);
}
void ALequalizer_setParamf(ALeffect *effect, ALCcontext *context, ALenum param, ALfloat val)
{
    ALeffectProps *props = &effect->Props;
    switch(param)
    {
        case AL_EQUALIZER_LOW_GAIN:
            if(!(val >= AL_EQUALIZER_MIN_LOW_GAIN && val <= AL_EQUALIZER_MAX_LOW_GAIN))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.LowGain = val;
            break;

        case AL_EQUALIZER_LOW_CUTOFF:
            if(!(val >= AL_EQUALIZER_MIN_LOW_CUTOFF && val <= AL_EQUALIZER_MAX_LOW_CUTOFF))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.LowCutoff = val;
            break;

        case AL_EQUALIZER_MID1_GAIN:
            if(!(val >= AL_EQUALIZER_MIN_MID1_GAIN && val <= AL_EQUALIZER_MAX_MID1_GAIN))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.Mid1Gain = val;
            break;

        case AL_EQUALIZER_MID1_CENTER:
            if(!(val >= AL_EQUALIZER_MIN_MID1_CENTER && val <= AL_EQUALIZER_MAX_MID1_CENTER))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.Mid1Center = val;
            break;

        case AL_EQUALIZER_MID1_WIDTH:
            if(!(val >= AL_EQUALIZER_MIN_MID1_WIDTH && val <= AL_EQUALIZER_MAX_MID1_WIDTH))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.Mid1Width = val;
            break;

        case AL_EQUALIZER_MID2_GAIN:
            if(!(val >= AL_EQUALIZER_MIN_MID2_GAIN && val <= AL_EQUALIZER_MAX_MID2_GAIN))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.Mid2Gain = val;
            break;

        case AL_EQUALIZER_MID2_CENTER:
            if(!(val >= AL_EQUALIZER_MIN_MID2_CENTER && val <= AL_EQUALIZER_MAX_MID2_CENTER))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.Mid2Center = val;
            break;

        case AL_EQUALIZER_MID2_WIDTH:
            if(!(val >= AL_EQUALIZER_MIN_MID2_WIDTH && val <= AL_EQUALIZER_MAX_MID2_WIDTH))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.Mid2Width = val;
            break;

        case AL_EQUALIZER_HIGH_GAIN:
            if(!(val >= AL_EQUALIZER_MIN_HIGH_GAIN && val <= AL_EQUALIZER_MAX_HIGH_GAIN))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.HighGain = val;
            break;

        case AL_EQUALIZER_HIGH_CUTOFF:
            if(!(val >= AL_EQUALIZER_MIN_HIGH_CUTOFF && val <= AL_EQUALIZER_MAX_HIGH_CUTOFF))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.HighCutoff = val;
            break;

        default:
            SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM);
    }
}
void ALequalizer_setParamfv(ALeffect *effect, ALCcontext *context, ALenum param, const ALfloat *vals)
{
    ALequalizer_setParamf(effect, context, param, vals[0]);
}

void ALequalizer_getParami(const ALeffect *UNUSED(effect), ALCcontext *context, ALenum UNUSED(param), ALint *UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
void ALequalizer_getParamiv(const ALeffect *effect, ALCcontext *context, ALenum param, ALint *vals)
{
    ALequalizer_getParami(effect, context, param, vals);
}
void ALequalizer_getParamf(const ALeffect *effect, ALCcontext *context, ALenum param, ALfloat *val)
{
    const ALeffectProps *props = &effect->Props;
    switch(param)
    {
        case AL_EQUALIZER_LOW_GAIN:
            *val = props->Equalizer.LowGain;
            break;

        case AL_EQUALIZER_LOW_CUTOFF:
            *val = props->Equalizer.LowCutoff;
            break;

        case AL_EQUALIZER_MID1_GAIN:
            *val = props->Equalizer.Mid1Gain;
            break;

        case AL_EQUALIZER_MID1_CENTER:
            *val = props->Equalizer.Mid1Center;
            break;

        case AL_EQUALIZER_MID1_WIDTH:
            *val = props->Equalizer.Mid1Width;
            break;

        case AL_EQUALIZER_MID2_GAIN:
            *val = props->Equalizer.Mid2Gain;
            break;

        case AL_EQUALIZER_MID2_CENTER:
            *val = props->Equalizer.Mid2Center;
            break;

        case AL_EQUALIZER_MID2_WIDTH:
            *val = props->Equalizer.Mid2Width;
            break;

        case AL_EQUALIZER_HIGH_GAIN:
            *val = props->Equalizer.HighGain;
            break;

        case AL_EQUALIZER_HIGH_CUTOFF:
            *val = props->Equalizer.HighCutoff;
            break;

        default:
            SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM);
    }
}
void ALequalizer_getParamfv(const ALeffect *effect, ALCcontext *context, ALenum param, ALfloat *vals)
{
    ALequalizer_getParamf(effect, context, param, vals);
}

DEFINE_ALEFFECT_VTABLE(ALequalizer);
