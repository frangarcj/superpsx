#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <audsrv.h>
#include <ps2_audio_driver.h>

#include "superpsx.h"
#include "spu.h"

#define LOG_TAG "SPU"

/* ---- SPU RAM (512 KB) ---- */
#define SPU_RAM_SIZE (512 * 1024)
static uint8_t spu_ram[SPU_RAM_SIZE];

/* ---- Voice state ---- */
#define SPU_NUM_VOICES 24

typedef struct {
    int16_t  vol_l;          /* Voice volume left */
    int16_t  vol_r;          /* Voice volume right */
    uint16_t pitch;          /* Sample rate (4096 = 44100 Hz) */
    uint16_t start_addr;     /* Start address in SPU RAM (in 8-byte units) */
    uint16_t adsr_lo;        /* ADSR parameter low */
    uint16_t adsr_hi;        /* ADSR parameter high */
    uint16_t adsr_level;     /* Current ADSR volume level */
    uint16_t repeat_addr;    /* Loop address in SPU RAM (in 8-byte units) */

    /* Runtime state (not directly mapped to registers) */
    int      active;         /* Voice is playing */
    uint32_t current_addr;   /* Current byte address in SPU RAM */
    uint32_t sample_pos;     /* Fixed-point position within decoded block (20.12) */
    int16_t  prev[2];        /* ADPCM decode history: prev[0]=s-1, prev[1]=s-2 */
    int16_t  decoded[28];    /* Decoded samples from current ADPCM block */
    int      block_decoded;  /* 1 = decoded[] is valid for current_addr */
} SPU_Voice;

static SPU_Voice voices[SPU_NUM_VOICES];

/* ---- Global SPU registers ---- */
static int16_t  main_vol_l;
static int16_t  main_vol_r;
static uint16_t key_on_lo;          /* KON low halfword (latched) */
static uint16_t key_on_hi;          /* KON high halfword (latched) */
static uint32_t endx;               /* ENDX flags (bits 0-23) */
static uint16_t spu_cnt;            /* SPUCNT */
static uint16_t spu_stat;           /* SPUSTAT */
static uint16_t transfer_addr;      /* Transfer address register (raw value) */
static uint32_t transfer_ptr;       /* Actual byte address in SPU RAM */
static uint16_t data_fifo;          /* Data transfer FIFO register */

/* Reverb / noise / misc registers — stored but not processed */
static uint16_t spu_reg_store[256];

/* ---- ADPCM filter coefficients (fixed-point, 1.0 = 64) ---- */
static const int32_t adpcm_filter[5][2] = {
    {   0,   0 },
    {  60,   0 },
    { 115, -52 },
    {  98, -55 },
    { 122, -60 },
};

/* ---- Audio output ---- */
#define SPU_SAMPLE_RATE   44100
#define SAMPLES_PER_FRAME 735   /* 44100 / 60 ≈ 735 */
#define SPU_MIX_BUF_SIZE  ((SAMPLES_PER_FRAME + 15) & ~15) /* Align to 16-byte boundary */

static int16_t mix_buffer[SPU_MIX_BUF_SIZE * 2] __attribute__((aligned(16))); /* Interleaved L/R */
static int32_t mix_buf_l[SPU_MIX_BUF_SIZE] __attribute__((aligned(16)));
static int32_t mix_buf_r[SPU_MIX_BUF_SIZE] __attribute__((aligned(16)));
static int spu_initialized = 0;

/* ---- Init / Shutdown ---- */
void SPU_Init(void)
{
    memset(spu_ram, 0, sizeof(spu_ram));
    memset(voices, 0, sizeof(voices));
    memset(spu_reg_store, 0, sizeof(spu_reg_store));
    main_vol_l = 0;
    main_vol_r = 0;
    key_on_lo = 0;
    key_on_hi = 0;
    endx = 0;
    spu_cnt = 0;
    spu_stat = 0;
    transfer_addr = 0;
    transfer_ptr = 0;

    int audio_ret = init_audio_driver();
    if (audio_ret < 0) {
        printf("[SPU] init_audio_driver failed: %d\n", audio_ret);
        return;
    }

    /* audsrv is initialized by the audio driver; configure format */
    struct audsrv_fmt_t fmt;
    fmt.freq = SPU_SAMPLE_RATE;
    fmt.bits = 16;
    fmt.channels = 2;
    int ret = audsrv_set_format(&fmt);
    if (ret != 0) {
        printf("[SPU] audsrv_set_format failed: %d\n", ret);
        return;
    }

    audsrv_set_volume(MAX_VOLUME);
    spu_initialized = 1;
    printf("[SPU] Initialized: %d Hz, 16-bit, stereo\n", SPU_SAMPLE_RATE);
}

void SPU_Shutdown(void)
{
    if (spu_initialized) {
        deinit_audio_driver();
        spu_initialized = 0;
        printf("[SPU] Shutdown\n");
    }
}

/* ---- Decode one 16-byte ADPCM block into 28 samples ---- */
/* Two-phase approach: (1) extract all nibbles branch-free, (2) tight IIR filter.
 * Filter-0 fast path: f0=f1=0 means no prediction → direct copy.             */
static void decode_adpcm_block(SPU_Voice *v, uint32_t addr)
{
    uint8_t *block = &spu_ram[addr & (SPU_RAM_SIZE - 1)];

    uint8_t shift_filter = block[0];
    uint8_t flags = block[1];
    int shift = shift_filter & 0x0F;
    int filter = (shift_filter >> 4) & 0x07;

    if (filter > 4) filter = 4;

    /* ---- Pass 1: Extract all 28 nibbles and pre-shift (branch-free) ---- */
    int16_t shifted[28];
    int i;
    for (i = 0; i < 14; i++) {
        uint8_t byte = block[2 + i];
        /* Low nibble (even sample): sign-extend 4-bit */
        int32_t lo = (int32_t)(int8_t)((byte & 0x0F) << 4) >> 4;
        /* High nibble (odd sample): sign-extend 4-bit */
        int32_t hi = (int32_t)(int8_t)(byte & 0xF0) >> 4;
        shifted[i * 2]     = (int16_t)((lo << 12) >> shift);
        shifted[i * 2 + 1] = (int16_t)((hi << 12) >> shift);
    }

    int16_t s_1, s_2;

    if (filter == 0) {
        /* ---- Fast path: no prediction (f0=f1=0) ---- */
        /* (s_1*0 + s_2*0 + 32) >> 6 == 0, so sample = shifted[i].
         * Pre-shifted values fit in int16_t (nibble −8..7, <<12 >>shift). */
        memcpy(v->decoded, shifted, sizeof(shifted));
        s_1 = shifted[27];
        s_2 = shifted[26];
    } else {
        /* ---- Pass 2: IIR filter with serial dependency ---- */
        int32_t f0 = adpcm_filter[filter][0];
        int32_t f1 = adpcm_filter[filter][1];

        s_1 = v->prev[0];
        s_2 = v->prev[1];

        for (i = 0; i < 28; i++) {
            int32_t sample = (int32_t)shifted[i]
                           + ((s_1 * f0 + s_2 * f1 + 32) >> 6);

            /* Clamp to 16-bit (overflow is rare → hint branch predictor) */
            if (__builtin_expect(sample > 32767, 0))  sample = 32767;
            if (__builtin_expect(sample < -32768, 0))  sample = -32768;

            v->decoded[i] = (int16_t)sample;
            s_2 = s_1;
            s_1 = (int16_t)sample;
        }
    }

    v->prev[0] = s_1;
    v->prev[1] = s_2;
    v->block_decoded = 1;

    /* Handle loop flags */
    if (flags & 0x04) {
        /* Loop start — set repeat address */
        v->repeat_addr = (uint16_t)(addr >> 3);
    }
    if (flags & 0x01) {
        /* Loop end */
        endx |= (1 << (v - voices));
        if (flags & 0x02) {
            /* Loop repeat: jump to repeat address */
            v->current_addr = (uint32_t)v->repeat_addr << 3;
        } else {
            /* No loop: stop voice */
            v->active = 0;
        }
    }
}

/* ---- Process Key On for all set bits ---- */
static void process_key_on(uint32_t kon)
{
    int i;
    for (i = 0; i < SPU_NUM_VOICES; i++) {
        if (kon & (1 << i)) {
            SPU_Voice *v = &voices[i];
            v->active = 1;
            v->current_addr = (uint32_t)v->start_addr << 3;
            v->sample_pos = 0;
            v->prev[0] = 0;
            v->prev[1] = 0;
            v->block_decoded = 0;
            v->adsr_level = 0x7FFF; /* Max volume (simplified ADSR) */
            endx &= ~(1 << i);     /* Clear ENDX bit on Key On */
            DLOG("Voice %d Key On: addr=0x%05" PRIX32 " pitch=0x%04X vol=%d/%d\n",
                 i, v->current_addr, v->pitch, v->vol_l, v->vol_r);
        }
    }
}

/* ---- Process Key Off for all set bits ---- */
static void process_key_off(uint32_t koff)
{
    int i;
    for (i = 0; i < SPU_NUM_VOICES; i++) {
        if (koff & (1 << i)) {
            voices[i].active = 0;
        }
    }
}

/* ---- Register I/O ---- */

void SPU_WriteReg(uint32_t offset, uint16_t value)
{
    /* Voice registers: 0x000-0xBF (24 voices × 8 half-word regs) */
    if (offset < 0xC0) {
        int voice = offset >> 3;
        int reg = offset & 0x07;
        if (voice < SPU_NUM_VOICES) {
            SPU_Voice *v = &voices[voice];
            switch (reg) {
            case 0: v->vol_l = (int16_t)value; break;
            case 1: v->vol_r = (int16_t)value; break;
            case 2: v->pitch = value; break;
            case 3: v->start_addr = value; break;
            case 4: v->adsr_lo = value; break;
            case 5: v->adsr_hi = value; break;
            case 6: v->adsr_level = value; break;
            case 7: v->repeat_addr = value; break;
            }
        }
        return;
    }

    /* Global registers (halfword offsets from 0x1F801C00) */
    switch (offset) {
    case 0xC0: /* Main Volume Left (0x1F801D80) */
        main_vol_l = (int16_t)value;
        break;
    case 0xC1: /* Main Volume Right (0x1F801D82) */
        main_vol_r = (int16_t)value;
        break;

    /* Key On (0x1F801D88-0x1F801D8B) — process immediately on each half */
    case 0xC4: /* KON low (0x1F801D88) */
        key_on_lo = value;
        if (value)
            process_key_on((uint32_t)value);
        break;
    case 0xC5: /* KON high (0x1F801D8A) */
        key_on_hi = value;
        if (value)
            process_key_on((uint32_t)value << 16);
        break;

    /* Key Off (0x1F801D8C-0x1F801D8F) — process immediately on each half */
    case 0xC6: /* KOFF low (0x1F801D8C) */
        if (value)
            process_key_off((uint32_t)value);
        break;
    case 0xC7: /* KOFF high (0x1F801D8E) */
        if (value)
            process_key_off((uint32_t)value << 16);
        break;

    /* ENDX (0x1F801D9C-0x1F801D9F) — read-only on real hardware, writes ignored */
    case 0xCE:
    case 0xCF:
        break;

    /* Transfer Address (0x1F801DA6) */
    case 0xD3:
        transfer_addr = value;
        transfer_ptr = (uint32_t)value << 3; /* ×8 */
        break;

    /* Data Transfer FIFO (0x1F801DA8) */
    case 0xD4:
        data_fifo = value;
        if (transfer_ptr < SPU_RAM_SIZE - 1) {
            spu_ram[transfer_ptr]     = (uint8_t)(value & 0xFF);
            spu_ram[transfer_ptr + 1] = (uint8_t)(value >> 8);
            transfer_ptr += 2;
            transfer_ptr &= (SPU_RAM_SIZE - 1);
        }
        break;

    /* SPUCNT (0x1F801DAA) */
    case 0xD5:
        spu_cnt = value;
        /* SPUSTAT bits 0-5 mirror SPUCNT bits 0-5 directly.
         * Bit 6 = IRQ9 flag (preserved). */
        spu_stat = (spu_stat & 0x0040) | (value & 0x3F);
        break;

    /* SPUSTAT (0x1F801DAE) — read-only, writes ignored */
    case 0xD7:
        break;

    default:
        /* Store in the generic register array for reads */
        if (offset < 256)
            spu_reg_store[offset] = value;
        break;
    }
}

uint16_t SPU_ReadReg(uint32_t offset)
{
    /* Voice registers */
    if (offset < 0xC0) {
        int voice = offset >> 3;
        int reg = offset & 0x07;
        if (voice < SPU_NUM_VOICES) {
            SPU_Voice *v = &voices[voice];
            switch (reg) {
            case 0: return (uint16_t)v->vol_l;
            case 1: return (uint16_t)v->vol_r;
            case 2: return v->pitch;
            case 3: return v->start_addr;
            case 4: return v->adsr_lo;
            case 5: return v->adsr_hi;
            case 6: return v->adsr_level;
            case 7: return v->repeat_addr;
            }
        }
        return 0;
    }

    switch (offset) {
    case 0xC0: return (uint16_t)main_vol_l;
    case 0xC1: return (uint16_t)main_vol_r;

    /* KON — reads return last written value */
    case 0xC4: return key_on_lo;
    case 0xC5: return key_on_hi;

    /* ENDX (0x1F801D9C) — NOT cleared on read */
    case 0xCE: return (uint16_t)(endx & 0xFFFF);
    case 0xCF: return (uint16_t)((endx >> 16) & 0xFFFF);

    case 0xD3: return transfer_addr;
    case 0xD5: return spu_cnt;
    case 0xD7: return spu_stat;

    default:
        if (offset < 256)
            return spu_reg_store[offset];
        return 0;
    }
}

/* ---- DMA Channel 4: CPU RAM ↔ SPU RAM ---- */
void SPU_DMA4(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    uint32_t block_size = bcr & 0xFFFF;
    uint32_t block_count = (bcr >> 16) & 0xFFFF;
    if (block_count == 0) block_count = 1;
    if (block_size == 0) block_size = 1;
    uint32_t total_words = block_size * block_count;
    uint32_t total_bytes = total_words * 4;

    uint32_t src_addr = madr & 0x1FFFFC;

    int direction = (chcr & 1); /* 0 = to main RAM, 1 = from main RAM (to SPU) */

    if (direction) {
        /* CPU RAM → SPU RAM */
        DLOG("DMA4 Write: CPU 0x%06" PRIX32 " -> SPU 0x%05" PRIX32 ", %" PRIu32 " words\n",
             src_addr, transfer_ptr, total_words);

        uint32_t i;
        for (i = 0; i < total_bytes; i += 2) {
            if (src_addr + i + 1 < PSX_RAM_SIZE && transfer_ptr < SPU_RAM_SIZE - 1) {
                spu_ram[transfer_ptr]     = psx_ram[src_addr + i];
                spu_ram[transfer_ptr + 1] = psx_ram[src_addr + i + 1];
                transfer_ptr += 2;
                transfer_ptr &= (SPU_RAM_SIZE - 1);
            }
        }
    } else {
        /* SPU RAM → CPU RAM */
        DLOG("DMA4 Read: SPU 0x%05" PRIX32 " -> CPU 0x%06" PRIX32 ", %" PRIu32 " words\n",
             transfer_ptr, src_addr, total_words);

        uint32_t i;
        for (i = 0; i < total_bytes; i += 2) {
            if (src_addr + i + 1 < PSX_RAM_SIZE && transfer_ptr < SPU_RAM_SIZE - 1) {
                psx_ram[src_addr + i]     = spu_ram[transfer_ptr];
                psx_ram[src_addr + i + 1] = spu_ram[transfer_ptr + 1];
                transfer_ptr += 2;
                transfer_ptr &= (SPU_RAM_SIZE - 1);
            }
        }
    }

    (void)chcr;
}

/* ---- Audio mixing and clamping (C Fallback) ---- */
static void SPU_Mix_And_Clamp(int32_t *mix_buf_l, int32_t *mix_buf_r, int16_t *out_buf, int num_samples, int vol_l, int vol_r)
{
    for (int i = 0; i < num_samples; i++)
    {
        /* Apply master volume */
        int32_t sl = (mix_buf_l[i] * vol_l) >> 14;
        int32_t sr = (mix_buf_r[i] * vol_r) >> 14;

        /* Clamping (Saturation) */
        if (sl > 32767) sl = 32767;
        else if (sl < -32768) sl = -32768;

        if (sr > 32767) sr = 32767;
        else if (sr < -32768) sr = -32768;

        /* Write to final interleaved output buffer (Stereo) */
        out_buf[i * 2]     = (int16_t)sl;
        out_buf[i * 2 + 1] = (int16_t)sr;
    }
}

/* ---- Audio mixing: generate one frame of samples ---- */
void SPU_GenerateSamples(void)
{
    if (!spu_initialized)
        return;

    /* Check if any voice is active — skip mixing entirely if silent */
    int any_active = 0;
    {
        for (int i = 0; i < SPU_NUM_VOICES; i++) {
            if (voices[i].active) {
                any_active = 1;
                break;
            }
        }
    }
    if (!any_active)
        return;

    /* Accumulate into intermediate 32-bit buffers */
    memset(mix_buf_l, 0, SPU_MIX_BUF_SIZE * sizeof(int32_t));
    memset(mix_buf_r, 0, SPU_MIX_BUF_SIZE * sizeof(int32_t));

    for (int i = 0; i < SPU_NUM_VOICES; i++) {
        SPU_Voice *v = &voices[i];
        if (!v->active)
            continue;

        for (int s = 0; s < SAMPLES_PER_FRAME; s++) {
            /* Decode block if needed */
            if (!v->block_decoded) {
                decode_adpcm_block(v, v->current_addr);
                if (!v->active)
                    break; /* Voice stopped by loop-end with no repeat */
            }

            /* Fetch sample from decoded buffer */
            uint32_t sample_idx = v->sample_pos >> 12;
            int16_t pcm = v->decoded[sample_idx];

            /* Apply voice volume (0x3FFF = max, 14-bit range) and accumulate */
            mix_buf_l[s] += ((int32_t)pcm * v->vol_l) >> 14;
            mix_buf_r[s] += ((int32_t)pcm * v->vol_r) >> 14;

            /* Advance sample position by pitch */
            v->sample_pos += v->pitch;

            /* Check if we've crossed into the next 28-sample block */
            if ((v->sample_pos >> 12) >= 28) {
                while ((v->sample_pos >> 12) >= 28) {
                    v->sample_pos -= (28 << 12);
                    v->current_addr += 16; /* 16 bytes per ADPCM block */
                    v->current_addr &= (SPU_RAM_SIZE - 1);
                    v->block_decoded = 0;
                }
                /* Decode next block immediately to handle flags and have valid samples */
                decode_adpcm_block(v, v->current_addr);
                if (!v->active)
                    break;
            }
        }
    }

    /* Perform final mix, volume apply and master clamp */
    SPU_Mix_And_Clamp(mix_buf_l, mix_buf_r, mix_buffer, SAMPLES_PER_FRAME, main_vol_l, main_vol_r);

    /* Output to audsrv */
    int size = SAMPLES_PER_FRAME * 2 * sizeof(int16_t);
    audsrv_wait_audio(size);
    audsrv_play_audio((char *)mix_buffer, size);
}
