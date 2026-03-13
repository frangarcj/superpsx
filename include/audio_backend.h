/**
 * audio_backend.h — Platform-agnostic audio output interface
 */
#ifndef AUDIO_BACKEND_H
#define AUDIO_BACKEND_H

#include <stdint.h>

/* Initialize platform audio subsystem. Returns 0 on success, <0 on error. */
int Audio_Backend_Init(void);

/* Configure audio format and volume. Returns 0 on success, <0 on error. */
int Audio_Backend_Configure(int sample_rate, int bits, int channels, int volume);

/* Submit audio samples for playback. */
void Audio_Backend_Play(const int16_t *buffer, int size_bytes);

/* Shutdown platform audio subsystem. */
void Audio_Backend_Shutdown(void);

#endif /* AUDIO_BACKEND_H */
