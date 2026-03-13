/**
 * audio_psp_backend.c — PSP audio output via sceAudio
 */
#include "audio_backend.h"
#include <pspaudio.h>
#include <string.h>

static int audio_channel = -1;
static int audio_volume = PSP_AUDIO_VOLUME_MAX;

int Audio_Backend_Init(void) {
    return 0; /* Channel allocated on first configure */
}

int Audio_Backend_Configure(int sample_rate, int bits, int channels, int volume) {
    (void)bits; /* PSP audio is always 16-bit */

    /* Release previous channel if any */
    if (audio_channel >= 0) {
        sceAudioChRelease(audio_channel);
        audio_channel = -1;
    }

    /* PSP audio works in fixed-size sample blocks.
     * sceAudioChReserve(channel, samplecount, format)
     * samplecount must be a multiple of 64, and 64-4096 range. */
    int format = (channels == 1) ? PSP_AUDIO_FORMAT_MONO : PSP_AUDIO_FORMAT_STEREO;
    int sample_count = 1024; /* 1024 samples per block */

    audio_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, sample_count, format);
    if (audio_channel < 0) return -1;

    audio_volume = (volume * PSP_AUDIO_VOLUME_MAX) / 100;
    if (audio_volume > PSP_AUDIO_VOLUME_MAX) audio_volume = PSP_AUDIO_VOLUME_MAX;

    (void)sample_rate; /* PSP outputs at 44100 — resampling needed if different */
    return 0;
}

void Audio_Backend_Play(const int16_t *buffer, int size_bytes) {
    if (audio_channel < 0 || !buffer || size_bytes <= 0) return;
    sceAudioOutputBlocking(audio_channel, audio_volume, (void *)buffer);
}

void Audio_Backend_Shutdown(void) {
    if (audio_channel >= 0) {
        sceAudioChRelease(audio_channel);
        audio_channel = -1;
    }
}
