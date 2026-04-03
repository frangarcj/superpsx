#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "audio_backend.h"
#include "superpsx.h"
#include "spu.h"
#include "scheduler.h"
#include "profiler.h"

#define LOG_TAG "SPU"

/* ---- SPU RAM (512 KB) ---- */
#define SPU_RAM_SIZE (512 * 1024)
static uint8_t spu_ram[SPU_RAM_SIZE];

/* ---- ADSR envelope phases ---- */
typedef enum
{
    ADSR_ATTACK = 0,
    ADSR_DECAY = 1,
    ADSR_SUSTAIN = 2,
    ADSR_RELEASE = 3,
    ADSR_OFF = 4
} AdsrPhase;

/* ---- Voice state ---- */
#define SPU_NUM_VOICES 24

typedef struct
{
    int16_t vol_l;        /* Voice volume left  (raw register, bit15=sweep) */
    int16_t vol_r;        /* Voice volume right (raw register, bit15=sweep) */
    uint16_t pitch;       /* Sample rate (4096 = 44100 Hz) */
    uint16_t start_addr;  /* Start address in SPU RAM (in 8-byte units) */
    uint16_t adsr_lo;     /* ADSR parameter low  (1F801C08+N*10h) */
    uint16_t adsr_hi;     /* ADSR parameter high (1F801C0Ah+N*10h) */
    uint16_t adsr_level;  /* Current ADSR volume level register (mirrors adsr_vol) */
    uint16_t repeat_addr; /* Loop address in SPU RAM (in 8-byte units) */

    /* Runtime state (not directly mapped to registers) */
    int active;            /* Voice is playing */
    uint32_t current_addr; /* Current byte address in SPU RAM */
    uint32_t sample_pos;   /* Fixed-point position within decoded block (20.12) */
    int16_t prev[2];       /* ADPCM decode history: prev[0]=s-1, prev[1]=s-2 */
    int16_t decoded[28];   /* Decoded samples from current ADPCM block */
    int block_decoded;     /* 1 = decoded[] is valid for current_addr */
    int loop_pending;      /* 1 = loop-end reached, block advance should jump to repeat_addr */
    int ignore_loop;       /* 1 = game wrote loop_addr register; ignore ADPCM flag 4 */

    /* ADSR envelope state */
    int32_t adsr_vol;     /* Current ADSR envelope volume: 0..0x7FFF */
    AdsrPhase adsr_phase; /* Current ADSR phase */
    int32_t adsr_counter; /* Envelope step counter (incremented each sample) */

    /* Cached ADSR tick parameters — precomputed on phase/register change, NOT per-sample */
    int32_t adsr_ci;   /* CounterIncrement for current phase (0..0x8000) */
    int32_t adsr_step; /* Signed step per counter overflow (direction applied) */
    int8_t adsr_exp;   /* 1 = current phase is exponential */
    int8_t adsr_inc;   /* 1 = current phase increases volume (0 = decreases) */
    int32_t adsr_sl;   /* Sustain level threshold (from adsr_lo[3:0]) */
} SPU_Voice;

static SPU_Voice voices[SPU_NUM_VOICES];

/* ---- Global SPU registers ---- */
static int16_t main_vol_l;
static int16_t main_vol_r;
static uint16_t key_on_lo;     /* KON low halfword (latched) */
static uint16_t key_on_hi;     /* KON high halfword (latched) */
static uint32_t endx;          /* ENDX flags (bits 0-23) */
static uint16_t spu_cnt;       /* SPUCNT */
static uint16_t spu_stat;      /* SPUSTAT */
static uint16_t transfer_addr; /* Transfer address register (raw value) */
static uint32_t transfer_ptr;  /* Actual byte address in SPU RAM */
static uint16_t data_fifo;     /* Data transfer FIFO register */

/* Deferred KON: voices are flagged for start, not started immediately.
 * Actual start happens in SPU_GenerateChunk.  If KOFF arrives before
 * mixing, it cancels the pending KON (matching real HW: KON wins when
 * KON and KOFF arrive in the same sample tick). */
static uint32_t pending_kon;   /* 24-bit mask of channels awaiting KON */

/* Reverb / noise / misc registers — stored but not processed */
static uint16_t spu_reg_store[256];

/* ---- SPU IRQ9 (address match interrupt) ---- */
static uint32_t spu_irq_addr;   /* Byte address in SPU RAM (IRQ address reg × 8) */
static int      spu_irq_enabled; /* Cached SPUCNT bit 6 */
static int      spu_irq_fired;   /* Sticky: set when IRQ9 fires, cleared on ack */

/* ---- ADPCM filter coefficients (fixed-point, 1.0 = 64) ---- */
static const int32_t adpcm_filter[5][2] = {
    {0, 0},
    {60, 0},
    {115, -52},
    {98, -55},
    {122, -60},
};

/* ---- Audio output ---- */
#define SPU_SAMPLE_RATE 44100
#define SPU_MAX_VOLUME  100
#define SAMPLES_PER_FRAME 735                             /* 44100 / 60 ≈ 735 */
#define SPU_MIX_BUF_SIZE ((SAMPLES_PER_FRAME + 15) & ~15) /* Align to 16-byte boundary */

static int16_t mix_buffer[SPU_MIX_BUF_SIZE * 2] __attribute__((aligned(16))); /* Interleaved L/R */
static int32_t mix_buf_l[SPU_MIX_BUF_SIZE] __attribute__((aligned(16)));
static int32_t mix_buf_r[SPU_MIX_BUF_SIZE] __attribute__((aligned(16)));
static int spu_initialized = 0;
int spu_samples_generated = 0; /* Incremental: how many samples generated this frame */
static uint64_t spu_frame_start_cycle = 0;

/* CPU cycles per SPU sample: 33868800 / 44100 ≈ 768 */
#define CYCLES_PER_SAMPLE 768

/* ---- ADSR envelope implementation ----
 *
 * PSX SPU ADSR algorithm (from psx-spx nocash documentation):
 *
 * Register layout:
 *   adsr_lo (1F801C08+N*10h):
 *     bit 15    = Attack Mode  (0=Linear, 1=Exponential)
 *     bits 14-10 = Attack Shift (0..1Fh, fast to slow)
 *     bits  9-8  = Attack Step  (0..3  = +7,+6,+5,+4)
 *     bits  7-4  = Decay Shift  (0..0Fh)
 *     bits  3-0  = Sustain Level (0..0Fh → threshold = (SL+1)<<11)
 *
 *   adsr_hi (1F801C0Ah+N*10h):
 *     bit 15    = Sustain Mode      (0=Linear, 1=Exponential)
 *     bit 14    = Sustain Direction (0=Increase, 1=Decrease)
 *     bit 13    = (unused)
 *     bits 12-8  = Sustain Shift    (0..1Fh)
 *     bits  7-6  = Sustain Step     (0..3)
 *     bit   5    = Release Mode     (0=Linear, 1=Exponential)
 *     bits  4-0  = Release Shift    (0..1Fh)
 *
 * Envelope step algorithm (per 44100Hz sample tick):
 *   AdsrStep         = (7 - step_val) << max(0, 11 - shift)
 *   CounterIncrement = 0x8000 >> max(0, shift - 11)
 *   Counter += CounterIncrement
 *   if (Counter & 0x8000): Counter &= 0x7FFF; AdsrLevel += AdsrStep
 *
 *   Exponential decrease: AdsrStep = AdsrStep * AdsrLevel / 0x8000
 *   Exponential increase (fake): when AdsrLevel > 0x6000, halve step (divide by 4 via sub-shift)
 */

/* Precompute adsr_ci / adsr_step / adsr_exp / adsr_inc / adsr_sl
 * from the current phase and register values.
 * Called ONCE per phase change — NOT per sample.                          */
static void adsr_reload_params(SPU_Voice *v)
{
    uint16_t lo = v->adsr_lo;
    uint16_t hi = v->adsr_hi;

    /* Sustain level threshold: same for all phases (from adsr_lo bits 3-0)
     * psx-spx: Level=(N+1)*800h. Cap to 0x7FFF so vol never exceeds the
     * valid envelope range — otherwise ENVX reads as 0 and games think
     * the voice is dead (e.g. Crash Bandicoot intro scream). */
    v->adsr_sl = (int32_t)(((lo & 0x0F) + 1) << 11);
    if (v->adsr_sl > 0x7FFF)
        v->adsr_sl = 0x7FFF;

    int shift, step_idx, is_exp, is_dec;

    switch (v->adsr_phase)
    {
    case ADSR_ATTACK:
        shift = (lo >> 10) & 0x1F;
        step_idx = (lo >> 8) & 0x03;
        is_exp = (lo >> 15) & 1;
        is_dec = 0;
        break;
    case ADSR_DECAY:
        shift = (lo >> 4) & 0x0F;
        step_idx = 0;
        is_exp = 1;
        is_dec = 1;
        break;
    case ADSR_SUSTAIN:
        shift = (hi >> 8) & 0x1F;
        step_idx = (hi >> 6) & 0x03;
        is_exp = (hi >> 15) & 1;
        is_dec = (hi >> 14) & 1;
        break;
    case ADSR_RELEASE:
        shift = hi & 0x1F;
        step_idx = 0;
        is_exp = (hi >> 5) & 1;
        is_dec = 1;
        break;
    default: /* ADSR_OFF */
        v->adsr_ci = 0;
        v->adsr_step = 0;
        v->adsr_exp = 0;
        v->adsr_inc = 0;
        return;
    }

    /* CounterIncrement = 0x8000 >> max(0, shift-11) */
    int ci_shift = shift - 11;
    v->adsr_ci = (ci_shift > 0) ? (0x8000 >> ci_shift) : 0x8000;

    /* BaseStep: psx-spx says increase="+7,+6,+5,+4", decrease="-8,-7,-6,-5"
     * for step_idx 0..3.  So increase uses (7-idx), decrease uses (8-idx). */
    int su = 11 - shift;
    int raw_step = is_dec ? (8 - step_idx) : (7 - step_idx);
    int32_t base = (su > 0) ? (raw_step << su) : raw_step;
    v->adsr_step = is_dec ? -base : base;
    v->adsr_exp = (int8_t)is_exp;
    v->adsr_inc = (int8_t)(!is_dec);
}

/* Advance ADSR envelope by one 44100 Hz sample tick.
 *
 * Hot path (counter hasn't overflowed): load ci, add, compare, return — ~4 ops.
 * Overflow path (rare): apply step, handle phase transitions.
 * Phase transitions call adsr_reload_params() to refresh cached params.    */
static inline void adsr_tick(SPU_Voice *v)
{
    /* Fake exponential increase: slow counter when volume is near max.
     * Checked outside the overflow branch to avoid ci modification issues. */
    int32_t ci = v->adsr_ci;
    if (__builtin_expect(v->adsr_exp & v->adsr_inc & (v->adsr_vol > 0x6000), 0))
        ci >>= 2;

    v->adsr_counter += ci;

    /* ---- Common fast path: counter has not overflowed ---- */
    if (__builtin_expect(v->adsr_counter < 0x8000, 1))
        return;

    /* ---- Overflow path (rare): apply envelope step ---- */
    v->adsr_counter &= 0x7FFF;

    int32_t vol = v->adsr_vol;
    int32_t step = v->adsr_step;

    /* Exponential decrease: scale step by current volume (32-bit, no int64) */
    if (__builtin_expect(v->adsr_exp & !v->adsr_inc, 0))
        step = (step * vol) >> 15;

    vol += step;

    /* Phase-specific clamping and transitions */
    switch (v->adsr_phase)
    {
    case ADSR_ATTACK:
        if (vol >= 0x7FFF || vol < 0)
        {
            vol = 0x7FFF;
            v->adsr_phase = ADSR_DECAY;
            v->adsr_counter = 0;
            adsr_reload_params(v);
        }
        break;
    case ADSR_DECAY:
        if (vol < 0)
            vol = 0;
        if (vol <= v->adsr_sl)
        {
            vol = v->adsr_sl;
            v->adsr_phase = ADSR_SUSTAIN;
            v->adsr_counter = 0;
            adsr_reload_params(v);
        }
        break;
    case ADSR_SUSTAIN:
        if (v->adsr_inc)
        {
            if (vol > 0x7FFF)
                vol = 0x7FFF;
        }
        else
        {
            if (vol < 0)
                vol = 0;
        }
        break;
    case ADSR_RELEASE:
        if (vol < 0)
            vol = 0;
        if (vol == 0)
        {
            v->adsr_phase = ADSR_OFF;
            v->active = 0;
        }
        break;
    default:
        break;
    }

    v->adsr_vol = vol;
    v->adsr_level = (uint16_t)((uint32_t)vol & 0x7FFF);
}

/* ---- Get effective voice volume (handle fixed vs sweep mode) ----
 * Bit 15 = 0: Fixed mode. Bits 14-0 are the volume (signed, -0x4000..+0x3FFF range
 *             but spec says 0x0000..0x3FFF for normal use; we take abs and treat as
 *             0..0x7FFF by left-shifting one bit).
 * Bit 15 = 1: Sweep mode. Bits 14-0 encode the sweep target/rate. For now, use bits
 *             6-0 as a coarse volume level (simplified — full sweep not implemented).
 *
 * Returns value in 0..0x7FFF range.
 */
static inline int32_t get_effective_volume(int16_t raw)
{
    if (raw & (int16_t)0x8000)
    {
        /* Sweep mode: bits 14-0 contain sweep parameters.
         * Use bits 13-7 (current level field) or just return max for now.
         * Proper sweep requires per-sample tracking; use a reasonable approximation. */
        int32_t v = ((int32_t)(raw & 0x7FFF)) << 1;
        if (v > 0x7FFF)
            v = 0x7FFF;
        return v;
    }
    else
    {
        /* Fixed mode: value is in -0x4000..+0x3FFF; left-shift to 0x7FFF base */
        int32_t v = (int32_t)(raw & 0x7FFF) << 1;
        if (v > 0x7FFF)
            v = 0x7FFF;
        if (v < 0)
            v = 0;
        return v;
    }
}

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
    pending_kon = 0;
    endx = 0;
    spu_cnt = 0;
    spu_stat = 0;
    transfer_addr = 0;
    transfer_ptr = 0;
    spu_irq_addr = 0;
    spu_irq_enabled = 0;
    spu_irq_fired = 0;

    int audio_ret = Audio_Backend_Init();
    if (audio_ret < 0)
    {
        printf("[SPU] Audio_Backend_Init failed: %d\n", audio_ret);
        return;
    }

    int ret = Audio_Backend_Configure(SPU_SAMPLE_RATE, 16, 2, SPU_MAX_VOLUME);
    if (ret != 0)
    {
        printf("[SPU] Audio_Backend_Configure failed: %d\n", ret);
        return;
    }

    spu_initialized = 1;
    printf("[SPU] Initialized: %d Hz, 16-bit, stereo\n", SPU_SAMPLE_RATE);
}

void SPU_Shutdown(void)
{
    if (spu_initialized)
    {
        Audio_Backend_Shutdown();
        spu_initialized = 0;
        printf("[SPU] Shutdown\n");
    }
}

/* ---- Decode one 16-byte ADPCM block into 28 samples ---- */
/* Two-phase approach: (1) extract all nibbles branch-free, (2) tight IIR filter.
 * Filter-0 fast path: f0=f1=0 means no prediction → direct copy.             */
static inline __attribute__((always_inline)) void decode_adpcm_block(SPU_Voice *v, uint32_t addr)
{
    uint8_t *block = &spu_ram[addr & (SPU_RAM_SIZE - 1)];

    uint8_t shift_filter = block[0];
    uint8_t flags = block[1];
    int shift = shift_filter & 0x0F;
    int filter = (shift_filter >> 4) & 0x07;

    if (filter > 4)
        filter = 4;

    /* ---- Pass 1: Extract all 28 nibbles and pre-shift (branch-free) ---- */
    int16_t shifted[28];
    int i;
    for (i = 0; i < 14; i++)
    {
        uint8_t byte = block[2 + i];
        /* Low nibble (even sample): sign-extend 4-bit */
        int32_t lo = (int32_t)(int8_t)((byte & 0x0F) << 4) >> 4;
        /* High nibble (odd sample): sign-extend 4-bit */
        int32_t hi = (int32_t)(int8_t)(byte & 0xF0) >> 4;
        shifted[i * 2] = (int16_t)((lo << 12) >> shift);
        shifted[i * 2 + 1] = (int16_t)((hi << 12) >> shift);
    }

    int16_t s_1, s_2;

    if (filter == 0)
    {
        /* ---- Fast path: no prediction (f0=f1=0) ---- */
        /* (s_1*0 + s_2*0 + 32) >> 6 == 0, so sample = shifted[i].
         * Pre-shifted values fit in int16_t (nibble −8..7, <<12 >>shift). */
        memcpy(v->decoded, shifted, sizeof(shifted));
        s_1 = shifted[27];
        s_2 = shifted[26];
    }
    else
    {
        /* ---- Pass 2: IIR filter with serial dependency ---- */
        int32_t f0 = adpcm_filter[filter][0];
        int32_t f1 = adpcm_filter[filter][1];

        s_1 = v->prev[0];
        s_2 = v->prev[1];

        for (i = 0; i < 28; i++)
        {
            int32_t sample = (int32_t)shifted[i] + ((s_1 * f0 + s_2 * f1 + 32) >> 6);

            /* Clamp to 16-bit (overflow is rare → hint branch predictor) */
            if (__builtin_expect(sample > 32767, 0))
                sample = 32767;
            if (__builtin_expect(sample < -32768, 0))
                sample = -32768;

            v->decoded[i] = (int16_t)sample;
            s_2 = s_1;
            s_1 = (int16_t)sample;
        }
    }

    v->prev[0] = s_1;
    v->prev[1] = s_2;
    v->block_decoded = 1;

    /* Handle loop flags */
    if ((flags & 0x04) && !v->ignore_loop)
    {
        /* Loop start — set repeat address */
        v->repeat_addr = (uint16_t)(addr >> 3);
    }
    if (flags & 0x01)
    {
        /* Loop end — set ENDX bit and schedule loop or stop.
         * Don't modify current_addr here; the block advance code will
         * apply the loop jump after all 28 samples are consumed. */
        endx |= (1 << (v - voices));
        if (flags & 0x02)
        {
            /* Loop repeat: defer jump to repeat_addr until block advance */
            v->loop_pending = 1;
        }
        else
        {
            /* No loop: stop voice after last sample of this block */
            v->loop_pending = 0;
            v->active = 0;
        }
    }
}

/* ---- Process Key On for all set bits ---- */
static void process_key_on(uint32_t kon)
{
    /* Bit-scan loop: skip directly to set bits instead of iterating all 24.
     * For typical KOFF values with 1-4 bits set, this is 20x fewer iterations. */
    while (kon)
    {
        int i = __builtin_ctz(kon);
        kon &= kon - 1; /* clear lowest set bit */
        if (i >= SPU_NUM_VOICES)
            break;
        {
            SPU_Voice *v = &voices[i];
            v->active = 1;
            v->current_addr = (uint32_t)(v->start_addr & ~1) << 3; /* Align to 16-byte ADPCM block */
            v->sample_pos = 0;
            v->prev[0] = 0;
            v->prev[1] = 0;
            v->block_decoded = 0;
            v->loop_pending = 0;
            v->ignore_loop = 0;
            /* Initialize ADSR envelope: Attack phase, volume 0 */
            v->adsr_vol = 0;
            v->adsr_phase = ADSR_ATTACK;
            v->adsr_counter = 0;
            v->adsr_level = 0;
            adsr_reload_params(v); /* Cache ADSR tick params for Attack phase */
            endx &= ~(1 << i);     /* Clear ENDX bit on Key On */
            DLOG("KON v%d: addr=0x%05" PRIX32 " pitch=0x%04X vol=%d/%d adsr=%04X/%04X\n",
                 i, v->current_addr, v->pitch, v->vol_l, v->vol_r, v->adsr_lo, v->adsr_hi);
        }
    }
}

/* ---- Flush deferred KON — called before generating samples or reading ENVX ---- */
static void flush_pending_kon(void)
{
    if (pending_kon) {
        process_key_on(pending_kon);
        pending_kon = 0;
    }
}

/* ---- Process Key Off for all set bits ---- */
static void process_key_off(uint32_t koff)
{
    while (koff)
    {
        int i = __builtin_ctz(koff);
        koff &= koff - 1;
        if (i >= SPU_NUM_VOICES)
            break;
        {
            SPU_Voice *v = &voices[i];
            if (v->active)
            {
                /* Transition to Release phase (do not stop immediately) */
                v->adsr_phase = ADSR_RELEASE;
                v->adsr_counter = 0;
                adsr_reload_params(v); /* Cache ADSR tick params for Release phase */
            }
        }
    }
}

/* ---- Register I/O ---- */

void SPU_WriteReg(uint32_t offset, uint16_t value)
{
    /* Voice registers: 0x000-0xBF (24 voices × 8 half-word regs) */
    if (offset < 0xC0)
    {
        int voice = offset >> 3;
        int reg = offset & 0x07;
        if (voice < SPU_NUM_VOICES)
        {
            SPU_Voice *v = &voices[voice];
            switch (reg)
            {
            case 0:
                v->vol_l = (int16_t)value;
                break;
            case 1:
                v->vol_r = (int16_t)value;
                break;
            case 2:
                v->pitch = value;
                break;
            case 3:
                v->start_addr = value;
                break;
            case 4:
                v->adsr_lo = value;
                if (v->active)
                    adsr_reload_params(v);
                break;
            case 5:
                v->adsr_hi = value;
                if (v->active)
                    adsr_reload_params(v);
                break;
            case 6:
                v->adsr_level = value;
                break;
            case 7:
                v->repeat_addr = value;
                v->ignore_loop = 1; /* Game explicitly set loop address */
                break;
            }
        }
        return;
    }

    /* Global registers (halfword offsets from 0x1F801C00) */
    switch (offset)
    {
    case 0xC0: /* Main Volume Left (0x1F801D80) */
        main_vol_l = (int16_t)value;
        break;
    case 0xC1: /* Main Volume Right (0x1F801D82) */
        main_vol_r = (int16_t)value;
        break;

    /* Key On (0x1F801D88-0x1F801D8B) — deferred to next mixing chunk */
    case 0xC4: /* KON low (0x1F801D88) */
        key_on_lo = value;
        if (value) {
            SPU_CatchUp();
            pending_kon |= (uint32_t)value & 0xFFFF;
        }
        break;
    case 0xC5: /* KON high (0x1F801D8A) */
        key_on_hi = value;
        if (value) {
            SPU_CatchUp();
            pending_kon |= ((uint32_t)value & 0xFF) << 16;
        }
        break;

    /* Key Off (0x1F801D8C-0x1F801D8F) — cancel pending KON, then release */
    case 0xC6: /* KOFF low (0x1F801D8C) */
        if (value) {
            SPU_CatchUp();
            /* Channels with pending KON: cancel the KOFF (KON wins) */
            uint32_t koff_lo = (uint32_t)value & ~pending_kon;
            /* Channels with pending KON that also get KOFF: cancel the KON too
             * (game wrote KON then KOFF before mixing → net effect = nothing) */
            pending_kon &= ~(uint32_t)value;
            if (koff_lo)
                process_key_off(koff_lo);
        }
        break;
    case 0xC7: /* KOFF high (0x1F801D8E) */
        if (value) {
            SPU_CatchUp();
            uint32_t koff_hi = ((uint32_t)value << 16) & ~pending_kon;
            pending_kon &= ~((uint32_t)value << 16);
            if (koff_hi)
                process_key_off(koff_hi);
        }
        break;

    /* ENDX (0x1F801D9C-0x1F801D9F) — writing any value clears all 24 ENDX bits (psx-spx) */
    case 0xCE:
    case 0xCF:
        SPU_CatchUp();
        endx = 0;
        break;

    /* Sound RAM IRQ Address (0x1F801DA4) */
    case 0xD2:
        spu_reg_store[0xD2] = value;
        spu_irq_addr = (uint32_t)value << 3; /* Convert from 8-byte units to byte address */
        break;

    /* Transfer Address (0x1F801DA6) */
    case 0xD3:
        transfer_addr = value;
        transfer_ptr = (uint32_t)value << 3; /* ×8 */
        break;

    /* Data Transfer FIFO (0x1F801DA8) */
    case 0xD4:
        data_fifo = value;
        if (transfer_ptr < SPU_RAM_SIZE - 1)
        {
            spu_ram[transfer_ptr] = (uint8_t)(value & 0xFF);
            spu_ram[transfer_ptr + 1] = (uint8_t)(value >> 8);
            /* Check IRQ9 address match on FIFO write */
            if (spu_irq_enabled && !spu_irq_fired &&
                (transfer_ptr & ~0xF) == spu_irq_addr)
            {
                spu_stat |= 0x0040;
                spu_irq_fired = 1;
                SignalInterrupt(9);
            }
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
        spu_irq_enabled = (value >> 6) & 1;
        /* Writing bit6=0 acknowledges/clears the IRQ flag */
        if (!spu_irq_enabled)
        {
            spu_stat &= ~0x0040;
            spu_irq_fired = 0;
        }
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
    if (offset < 0xC0)
    {
        int voice = offset >> 3;
        int reg = offset & 0x07;
        if (voice < SPU_NUM_VOICES)
        {
            SPU_Voice *v = &voices[voice];
            switch (reg)
            {
            case 0:
                return (uint16_t)v->vol_l;
            case 1:
                return (uint16_t)v->vol_r;
            case 2:
                return v->pitch;
            case 3:
                return v->start_addr;
            case 4:
                return v->adsr_lo;
            case 5:
                return v->adsr_hi;
            case 6:
                /* ENVX: catch up SPU so envelope reflects current cycle.
                 * Without this, games polling ENVX right after Key On see 0
                 * and assume the voice is dead (missing SFX). */
                SPU_CatchUp();
                return v->adsr_level;
            case 7:
                return v->repeat_addr;
            }
        }
        return 0;
    }

    switch (offset)
    {
    case 0xC0:
        return (uint16_t)main_vol_l;
    case 0xC1:
        return (uint16_t)main_vol_r;

    /* KON — reads return last written value */
    case 0xC4:
        return key_on_lo;
    case 0xC5:
        return key_on_hi;

    /* ENDX (0x1F801D9C) — catch up so game sees current voice-done status */
    case 0xCE:
        SPU_CatchUp();
        return (uint16_t)(endx & 0xFFFF);
    case 0xCF:
        SPU_CatchUp();
        return (uint16_t)((endx >> 16) & 0xFFFF);

    case 0xD3:
        return transfer_addr;
    case 0xD5:
        return spu_cnt;
    case 0xD7:
        return spu_stat;

    default:
        if (offset < 256)
            return spu_reg_store[offset];
        return 0;
    }
}

/* ---- DMA Channel 4: CPU RAM ↔ SPU RAM ---- */
void SPU_DMA4(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    /* Catch up SPU before modifying SPU RAM — voices reading from
     * the transfer region need their samples generated first. */
    SPU_CatchUp();

    uint32_t block_size = bcr & 0xFFFF;
    uint32_t block_count = (bcr >> 16) & 0xFFFF;
    if (block_count == 0)
        block_count = 1;
    if (block_size == 0)
        block_size = 1;
    uint32_t total_words = block_size * block_count;
    uint32_t total_bytes = total_words * 4;

    uint32_t src_addr = madr & 0x1FFFFC;

    int direction = (chcr & 1); /* 0 = to main RAM, 1 = from main RAM (to SPU) */

    if (direction)
    {
        /* CPU RAM → SPU RAM */
        DLOG("DMA4 Write: CPU 0x%06" PRIX32 " -> SPU 0x%05" PRIX32 ", %" PRIu32 " words\n",
             src_addr, transfer_ptr, total_words);

        uint32_t i;
        for (i = 0; i < total_bytes; i += 2)
        {
            if (src_addr + i + 1 < PSX_RAM_SIZE && transfer_ptr < SPU_RAM_SIZE - 1)
            {
                spu_ram[transfer_ptr] = psx_ram[src_addr + i];
                spu_ram[transfer_ptr + 1] = psx_ram[src_addr + i + 1];
                /* SPU IRQ9: check if DMA write touches IRQ address */
                if (spu_irq_enabled && !spu_irq_fired &&
                    (transfer_ptr & ~0xF) == spu_irq_addr)
                {
                    spu_stat |= 0x0040;
                    spu_irq_fired = 1;
                    SignalInterrupt(9);
                }
                transfer_ptr += 2;
                transfer_ptr &= (SPU_RAM_SIZE - 1);
            }
        }
    }
    else
    {
        /* SPU RAM → CPU RAM */
        DLOG("DMA4 Read: SPU 0x%05" PRIX32 " -> CPU 0x%06" PRIX32 ", %" PRIu32 " words\n",
             transfer_ptr, src_addr, total_words);

        uint32_t i;
        for (i = 0; i < total_bytes; i += 2)
        {
            if (src_addr + i + 1 < PSX_RAM_SIZE && transfer_ptr < SPU_RAM_SIZE - 1)
            {
                psx_ram[src_addr + i] = spu_ram[transfer_ptr];
                psx_ram[src_addr + i + 1] = spu_ram[transfer_ptr + 1];
                /* SPU IRQ9: check if DMA read touches IRQ address */
                if (spu_irq_enabled && !spu_irq_fired &&
                    (transfer_ptr & ~0xF) == spu_irq_addr)
                {
                    spu_stat |= 0x0040;
                    spu_irq_fired = 1;
                    SignalInterrupt(9);
                }
                transfer_ptr += 2;
                transfer_ptr &= (SPU_RAM_SIZE - 1);
            }
        }
    }

    (void)chcr;
}

/* ---- Audio mixing and clamping ---- */
/* restrict: ml/mr/out never alias; branchless ternary clamp → conditional moves */
static void SPU_Mix_And_Clamp(int32_t *__restrict__ ml, int32_t *__restrict__ mr,
                              int16_t *__restrict__ out, int num_samples,
                              int vol_l, int vol_r)
{
    for (int i = 0; i < num_samples; i++)
    {
        /* vol_l/vol_r are in 0x7FFF range; normalize with >>15 */
        int32_t sl = (ml[i] * vol_l) >> 15;
        int32_t sr = (mr[i] * vol_r) >> 15;

        /* Branchless clamp — compiler emits slt+movn/movz on MIPS */
        sl = (sl > 32767) ? 32767 : (sl < -32768) ? -32768
                                                  : sl;
        sr = (sr > 32767) ? 32767 : (sr < -32768) ? -32768
                                                  : sr;

        out[i * 2] = (int16_t)sl;
        out[i * 2 + 1] = (int16_t)sr;
    }
}

/* ---- Audio mixing: generate a chunk of samples into the mix buffers ---- */
/* Called incrementally during HBlank batches to spread CPU load.           */
void SPU_GenerateChunk(int num_samples)
{
    if (!spu_initialized || num_samples <= 0)
        return;

    PROF_PUSH(PROF_SPU_MIX);

    int offset = spu_samples_generated;
    if (offset + num_samples > SAMPLES_PER_FRAME)
        num_samples = SAMPLES_PER_FRAME - offset;
    if (num_samples <= 0)
    {
        PROF_POP(PROF_SPU_MIX);
        return;
    }

    /* Clear the chunk region in mix buffers */
    memset(&mix_buf_l[offset], 0, num_samples * sizeof(int32_t));
    memset(&mix_buf_r[offset], 0, num_samples * sizeof(int32_t));

    for (int i = 0; i < SPU_NUM_VOICES; i++)
    {
        SPU_Voice *v = &voices[i];
        if (!v->active)
            continue;

        int32_t v_vol_l = get_effective_volume(v->vol_l);
        int32_t v_vol_r = get_effective_volume(v->vol_r);
        uint32_t v_pitch = v->pitch;

        /* Skip entirely silent voices (zero volume on both channels) */
        if (__builtin_expect(v_vol_l == 0 && v_vol_r == 0, 0))
            continue;

        int32_t last_adsr_vol = v->adsr_vol;
        int32_t comb_vol_l = (v_vol_l * last_adsr_vol) >> 15;
        int32_t comb_vol_r = (v_vol_r * last_adsr_vol) >> 15;

        int s = 0;
        while (s < num_samples && v->active)
        {
            if (__builtin_expect(!v->block_decoded, 0))
            {
                /* SPU IRQ9: check if voice is reading from the IRQ address */
                if (spu_irq_enabled && !spu_irq_fired &&
                    v->current_addr == spu_irq_addr)
                {
                    spu_stat |= 0x0040;
                    spu_irq_fired = 1;
                    SignalInterrupt(9);
                }
                decode_adpcm_block(v, v->current_addr);
                if (!v->active)
                    break;
            }

            /* ---- Compute safe batch: samples with constant ADSR volume ---- */
            int32_t ci = v->adsr_ci;
            if (__builtin_expect(v->adsr_exp & v->adsr_inc & (v->adsr_vol > 0x6000), 0))
                ci >>= 2;
            if (ci <= 0)
                ci = 1;

            /* Samples before ADSR counter overflows (volume stays constant) */
            int batch = (v->adsr_counter < 0x8000)
                            ? ((0x7FFF - v->adsr_counter) / ci)
                            : 0;

            /* Limit by decoded ADPCM block boundary */
            if (v_pitch > 0)
            {
                int blk_lim = ((28 << 12) - (int)v->sample_pos - 1) / (int)v_pitch + 1;
                if (blk_lim > 0 && blk_lim < batch)
                    batch = blk_lim;
            }

            /* Limit by remaining samples */
            int remain = num_samples - s;
            if (batch > remain)
                batch = remain;

            /* ---- Tight batch loop: constant volume, no ADSR tick ---- */
            for (int b = 0; b < batch; b++)
            {
                uint32_t si = v->sample_pos >> 12;
                int16_t pcm = v->decoded[si];
                mix_buf_l[offset + s] += ((int32_t)pcm * comb_vol_l) >> 15;
                mix_buf_r[offset + s] += ((int32_t)pcm * comb_vol_r) >> 15;
                v->sample_pos += v_pitch;
                s++;
            }
            v->adsr_counter += ci * batch;

            /* Handle ADPCM block boundary */
            if (__builtin_expect((v->sample_pos >> 12) >= 28, 0))
            {
                do
                {
                    v->sample_pos -= (28 << 12);
                    if (__builtin_expect(v->loop_pending, 0))
                    {
                        v->current_addr = (uint32_t)v->repeat_addr << 3;
                        v->loop_pending = 0;
                    }
                    else
                    {
                        v->current_addr += 16;
                        v->current_addr &= (SPU_RAM_SIZE - 1);
                    }
                } while ((v->sample_pos >> 12) >= 28);
                v->block_decoded = 0;
            }

            if (s >= num_samples)
                break;

            /* ---- Single sample with full ADSR tick (handles overflow) ---- */
            if (__builtin_expect(!v->block_decoded, 0))
            {
                /* SPU IRQ9: check address match */
                if (spu_irq_enabled && !spu_irq_fired &&
                    v->current_addr == spu_irq_addr)
                {
                    spu_stat |= 0x0040;
                    spu_irq_fired = 1;
                    SignalInterrupt(9);
                }
                decode_adpcm_block(v, v->current_addr);
                if (!v->active)
                    break;
            }

            adsr_tick(v);
            if (v->adsr_phase == ADSR_OFF)
            {
                v->active = 0;
                break;
            }

            if (__builtin_expect(v->adsr_vol != last_adsr_vol, 0))
            {
                last_adsr_vol = v->adsr_vol;
                comb_vol_l = (v_vol_l * last_adsr_vol) >> 15;
                comb_vol_r = (v_vol_r * last_adsr_vol) >> 15;
            }

            {
                uint32_t si = v->sample_pos >> 12;
                int16_t pcm = v->decoded[si];
                mix_buf_l[offset + s] += ((int32_t)pcm * comb_vol_l) >> 15;
                mix_buf_r[offset + s] += ((int32_t)pcm * comb_vol_r) >> 15;
                v->sample_pos += v_pitch;
                s++;
            }

            if (__builtin_expect((v->sample_pos >> 12) >= 28, 0))
            {
                do
                {
                    v->sample_pos -= (28 << 12);
                    if (__builtin_expect(v->loop_pending, 0))
                    {
                        v->current_addr = (uint32_t)v->repeat_addr << 3;
                        v->loop_pending = 0;
                    }
                    else
                    {
                        v->current_addr += 16;
                        v->current_addr &= (SPU_RAM_SIZE - 1);
                    }
                } while ((v->sample_pos >> 12) >= 28);
                v->block_decoded = 0;
            }
        }
    }

    spu_samples_generated += num_samples;
    PROF_POP(PROF_SPU_MIX);
}

/* ---- Flush accumulated samples to audio hardware ---- */
void SPU_FlushAudio(void)
{
    if (!spu_initialized)
        return;

    PROF_PUSH(PROF_SPU_FLUSH);

    int total = spu_samples_generated;
    if (total <= 0)
    {
        spu_samples_generated = 0;
        PROF_POP(PROF_SPU_FLUSH);
        return;
    }

    /* Apply main volume and clamp */
    int32_t eff_vol_l = get_effective_volume(main_vol_l);
    int32_t eff_vol_r = get_effective_volume(main_vol_r);

    SPU_Mix_And_Clamp(mix_buf_l, mix_buf_r, mix_buffer, total, eff_vol_l, eff_vol_r);

    /* Output to audsrv — non-blocking: IOP-side play_audio copies
     * whatever fits into the ring buffer (MIN(size, available)) and
     * returns immediately.  Removing the blocking wait_audio() saves
     * ~3ms/frame that was spent sleeping on the IOP queue semaphore.
     * If the ring buffer is momentarily full, samples are silently
     * dropped — acceptable trade-off for emulation speed. */
    int size = total * 2 * sizeof(int16_t);
    Audio_Backend_Play(mix_buffer, size);

    spu_samples_generated = 0;
    PROF_POP(PROF_SPU_FLUSH);
}

/* ---- Catch-up: generate pending samples up to current cycle ---- */
void SPU_CatchUp(void)
{
    if (prof_disable_spu || !spu_initialized)
        return;

    uint64_t now = global_cycles + partial_block_cycles;
    if (now <= spu_frame_start_cycle)
        return;

    int target = (int)((now - spu_frame_start_cycle) / CYCLES_PER_SAMPLE);

    /* If the current frame's audio is fully generated but more cycles have
     * elapsed (e.g. KON arriving during a long ISR after VBlank), flush the
     * completed frame and advance spu_frame_start_cycle so new voices can
     * produce samples immediately instead of waiting for VBlank. */
    if (target > SAMPLES_PER_FRAME && spu_samples_generated >= SAMPLES_PER_FRAME)
    {
        SPU_FlushAudio(); /* sends current frame to audio backend + resets gen=0 */
        spu_frame_start_cycle += (uint64_t)SAMPLES_PER_FRAME * CYCLES_PER_SAMPLE;
        target = (int)((now - spu_frame_start_cycle) / CYCLES_PER_SAMPLE);
    }

    if (target > SAMPLES_PER_FRAME)
        target = SAMPLES_PER_FRAME;

    int pending = target - spu_samples_generated;
    if (pending > 0) {
        flush_pending_kon();
        SPU_GenerateChunk(pending);
    }
}

/* ---- Frame start: reset cycle tracker for catch-up ---- */
void SPU_FrameStart(void)
{
    spu_frame_start_cycle = global_cycles;
}

/* ---- Generate remaining samples for frame and flush to audio hw ---- */
void SPU_GenerateSamples(void)
{
    /* Profiling: skip SPU entirely when disabled */
    if (prof_disable_spu)
    {
        spu_samples_generated = 0;
        return;
    }

    /* Generate any remaining samples for this frame */
    flush_pending_kon();
    int remaining = SAMPLES_PER_FRAME - spu_samples_generated;
    if (remaining > 0)
        SPU_GenerateChunk(remaining);
    SPU_FlushAudio();
}
