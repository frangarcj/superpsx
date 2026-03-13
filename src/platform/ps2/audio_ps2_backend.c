/**
 * audio_ps2_backend.c — Audio_Backend_* implementation for PS2
 *
 * Bridges audio_backend.h to PS2 audsrv/IOP audio driver.
 */
#include "audio_backend.h"
#include <audsrv.h>
#include <ps2_audio_driver.h>
#include <stdio.h>

int Audio_Backend_Init(void)
{
    return init_audio_driver();
}

int Audio_Backend_Configure(int sample_rate, int bits, int channels, int volume)
{
    struct audsrv_fmt_t fmt;
    fmt.freq = sample_rate;
    fmt.bits = bits;
    fmt.channels = channels;

    int ret = audsrv_set_format(&fmt);
    if (ret != 0)
        return ret;

    audsrv_set_volume(volume);
    return 0;
}

void Audio_Backend_Play(const int16_t *buffer, int size_bytes)
{
    audsrv_play_audio((char *)buffer, size_bytes);
}

void Audio_Backend_Shutdown(void)
{
    deinit_audio_driver();
}
