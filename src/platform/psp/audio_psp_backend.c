/**
 * audio_psp_backend.c — PSP audio output via sceAudio
 */
#include "audio_backend.h"
#include <pspaudio.h>
#include <string.h>

static int audio_channel = -1;
static int audio_volume = PSP_AUDIO_VOLUME_MAX;

#define PSP_AUDIO_BLOCK_SAMPLES 512
static int16_t audio_internal_buf[2048 * 2] __attribute__((aligned(64)));
static int audio_buf_samples = 0;

int Audio_Backend_Init(void) {
    audio_buf_samples = 0;
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
    
    /* We use a fixed block size of 512 samples for stability */
    audio_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, PSP_AUDIO_BLOCK_SAMPLES, format);
    if (audio_channel < 0) return -1;

    audio_volume = (volume * PSP_AUDIO_VOLUME_MAX) / 100;
    if (audio_volume > PSP_AUDIO_VOLUME_MAX) audio_volume = PSP_AUDIO_VOLUME_MAX;

    audio_buf_samples = 0;

    (void)sample_rate; /* PSP outputs at 44100 — resampling needed if different */
    return 0;
}

void Audio_Backend_Play(const int16_t *buffer, int size_bytes) {
    if (audio_channel < 0 || !buffer || size_bytes <= 0) return;

    int incoming_samples = size_bytes / (2 * sizeof(int16_t)); /* Assuming stereo */
    int samples_processed = 0;

    while (samples_processed < incoming_samples) {
        int to_copy = PSP_AUDIO_BLOCK_SAMPLES - audio_buf_samples;
        if (to_copy > (incoming_samples - samples_processed))
            to_copy = incoming_samples - samples_processed;

        memcpy(&audio_internal_buf[audio_buf_samples * 2], 
               &buffer[samples_processed * 2], 
               to_copy * 2 * sizeof(int16_t));

        audio_buf_samples += to_copy;
        samples_processed += to_copy;

        if (audio_buf_samples >= PSP_AUDIO_BLOCK_SAMPLES) {
            /* Output the full block. Blocking call ensures we don't drop samples. */
            sceAudioOutputBlocking(audio_channel, audio_volume, audio_internal_buf);
            audio_buf_samples = 0;
        }
    }
}

void Audio_Backend_Shutdown(void) {
    if (audio_channel >= 0) {
        sceAudioChRelease(audio_channel);
        audio_channel = -1;
    }
    audio_buf_samples = 0;
}
