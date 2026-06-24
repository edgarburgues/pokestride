/*
 * PokeWalker beeper emulation via 3DS NDSP.
 *
 * The ROM uses Timer W to generate beeps. GRA holds the compare value.
 * Timer W runs on the sub-clock (32768 Hz) with CKS=4 (TCRW=0xC0),
 * giving freq = 32768 / (2 * GRA). setBeeperFreq takes a uint8_t period
 * as the GRA value; e.g. GRA=128 -> 128 Hz, GRA=64 -> 256 Hz, GRA=32 -> 512 Hz.
 *
 * AUDIO_DISABLED master switch: set to 1 to keep all audio code inert —
 * NDSP is never initialised, no samples are generated, no DSP channel is
 * touched. The beeper code below is left intact so it can be re-enabled by
 * setting this back to 0.
 */
#define AUDIO_DISABLED 1

#include <3ds.h>
#include <string.h>
#include <stdlib.h>

#include "audio.h"

#define SAMPLE_RATE     22050
#define SUB_CLOCK       32768       /* Timer W clock source (phi_w) */
#define BEEP_MS         80
#define BEEP_SAMPLES    (SAMPLE_RATE * BEEP_MS / 1000)
#define BUF_SAMPLES     (SAMPLE_RATE / 4)   /* 250ms max */
#define AMPLITUDE       0x5000
#define NDSP_CHN        0

#if !AUDIO_DISABLED
static s16 *audioBuffer = NULL;     /* Must be in linear memory for DSP DMA */
static ndspWaveBuf waveBuf;
static bool audioInited = false;
#endif

void audioInit(void) {
#if AUDIO_DISABLED
    /* Audio is intentionally disabled — see AUDIO_DISABLED comment above. */
    return;
#else
    Result rc = ndspInit();
    if (rc != 0) return;
    audioInited = true;

    /* Allocate audio buffer in linear memory (required for NDSP DMA) */
    audioBuffer = (s16 *)linearMemAlign(BUF_SAMPLES * sizeof(s16), 4);
    if (!audioBuffer) {
        ndspExit();
        audioInited = false;
        return;
    }

    ndspSetOutputMode(NDSP_OUTPUT_MONO);
    ndspChnReset(NDSP_CHN);
    ndspChnSetInterp(NDSP_CHN, NDSP_INTERP_NONE);
    ndspChnSetRate(NDSP_CHN, (float)SAMPLE_RATE);
    ndspChnSetFormat(NDSP_CHN, NDSP_FORMAT_MONO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(NDSP_CHN, mix);

    memset(&waveBuf, 0, sizeof(waveBuf));
    memset(audioBuffer, 0, BUF_SAMPLES * sizeof(s16));
    waveBuf.data_vaddr = audioBuffer;
    waveBuf.looping = false;
    waveBuf.status = NDSP_WBUF_FREE;
#endif /* AUDIO_DISABLED */
}

bool audioIsReady(void) {
#if AUDIO_DISABLED
    return false;
#else
    return audioInited;
#endif
}

void audioExit(void) {
#if AUDIO_DISABLED
    return;
#else
    if (!audioInited) return;
    ndspChnWaveBufClear(NDSP_CHN);
    ndspExit();
    if (audioBuffer) {
        linearFree(audioBuffer);
        audioBuffer = NULL;
    }
    audioInited = false;
#endif
}

void audioUpdate(uint16_t graValue, uint8_t volume, bool timerActive) {
#if AUDIO_DISABLED
    (void)graValue; (void)volume; (void)timerActive;
    return;
#else
    (void)timerActive;
    if (!audioInited || !audioBuffer) return;
    if (volume == 0 || graValue == 0) return;

    /* Frequency: sub-clock / (2 * GRA) */
    uint32_t freq = SUB_CLOCK / (2 * (uint32_t)graValue);
    if (freq < 20 || freq > 10000) return;  /* sanity */

    /* Half-period in samples */
    uint32_t halfPeriod = SAMPLE_RATE / (2 * freq);
    if (halfPeriod < 1) halfPeriod = 1;
    uint32_t fullPeriod = halfPeriod * 2;

    /* Volume scaling: ROM range is 0-2, max volume = 2 */
    s16 amp = (s16)(AMPLITUDE * volume / 2);

    uint32_t numSamples = BEEP_SAMPLES;
    if (numSamples > BUF_SAMPLES) numSamples = BUF_SAMPLES;

    for (uint32_t i = 0; i < numSamples; i++) {
        audioBuffer[i] = ((i % fullPeriod) < halfPeriod) ? amp : -amp;
    }
    for (uint32_t i = numSamples; i < BUF_SAMPLES; i++) {
        audioBuffer[i] = 0;
    }

    DSP_FlushDataCache(audioBuffer, BUF_SAMPLES * sizeof(s16));

    ndspChnWaveBufClear(NDSP_CHN);
    waveBuf.nsamples = numSamples;
    waveBuf.status = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(NDSP_CHN, &waveBuf);
#endif /* AUDIO_DISABLED */
}
