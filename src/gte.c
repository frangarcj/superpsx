/*
 * SuperPSX – Comprehensive GTE (Geometry Transformation Engine) Emulation
 *
 * Based on the PSX-SPX documentation:
 *   https://psx-spx.consoledev.net/geometrytransformationenginegte/
 *
 * Implements all 22 GTE opcodes tested by ps1-tests gte/test-all:
 *   RTPS, NCLIP, OP, DPCS, INTPL, MVMVA, NCDS, CDP, NCDT, NCCS, CC,
 *   NCCT, NCS, NCT, SQR, DCPL, DPCT, AVSZ3, AVSZ4, RTPT, GPF, GPL
 */

#include "superpsx.h"
#include <stdio.h>
#include <string.h>

#ifdef ENABLE_HOST_LOG
extern FILE *host_log_file;
#define DBG(...)                                 \
    do                                           \
    {                                            \
        if (host_log_file)                       \
        {                                        \
            fprintf(host_log_file, __VA_ARGS__); \
            fflush(host_log_file);               \
        }                                        \
    } while (0)
#else
#define DBG(...)
#endif

/* ================================================================
 * UNR Division Table (for RTPS/RTPT)
 * Generated as: unr_table[i] = min(0, (0x40000/(i+0x100)+1)/2 - 0x101)
 * ================================================================ */
static const uint8_t unr_table[0x101] = {
    0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3,
    0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5, 0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8,
    0xC6, 0xC5, 0xC3, 0xC1, 0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0,
    0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x9A,
    0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8F, 0x8D, 0x8C, 0x8B, 0x8A, 0x89, 0x87, 0x86,
    0x85, 0x84, 0x83, 0x82, 0x81, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74,
    0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
    0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D, 0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55,
    0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x48,
    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3F, 0x3F, 0x3E, 0x3D, 0x3C, 0x3C, 0x3B,
    0x3A, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2F,
    0x2E, 0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
    0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1F, 0x1E, 0x1E, 0x1D, 0x1D, 0x1C, 0x1B, 0x1B, 0x1A,
    0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
    0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x08, 0x08,
    0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
    0x00 /* extra entry for index 0x100 */
};

/* ================================================================
 * GTE State  (all fields accessed via cpu->cp2_data / cpu->cp2_ctrl)
 *
 * Data registers 0..31  -> cpu->cp2_data[0..31]
 * Control registers 0..31 -> cpu->cp2_ctrl[0..31]
 * ================================================================ */

/* ---- Handy shorter names ---- */
#define D(n) cpu->cp2_data[(n)]
#define C(n) cpu->cp2_ctrl[(n)]

/* Data register indices */
enum
{
    d_VXY0 = 0,
    d_VZ0 = 1,
    d_VXY1 = 2,
    d_VZ1 = 3,
    d_VXY2 = 4,
    d_VZ2 = 5,
    d_RGBC = 6,
    d_OTZ = 7,
    d_IR0 = 8,
    d_IR1 = 9,
    d_IR2 = 10,
    d_IR3 = 11,
    d_SXY0 = 12,
    d_SXY1 = 13,
    d_SXY2 = 14,
    d_SXYP = 15,
    d_SZ0 = 16,
    d_SZ1 = 17,
    d_SZ2 = 18,
    d_SZ3 = 19,
    d_RGB0 = 20,
    d_RGB1 = 21,
    d_RGB2 = 22,
    d_RES1 = 23,
    d_MAC0 = 24,
    d_MAC1 = 25,
    d_MAC2 = 26,
    d_MAC3 = 27,
    d_IRGB = 28,
    d_ORGB = 29,
    d_LZCS = 30,
    d_LZCR = 31
};

/* Control register indices */
enum
{
    c_RT11RT12 = 0,
    c_RT13RT21 = 1,
    c_RT22RT23 = 2,
    c_RT31RT32 = 3,
    c_RT33 = 4,
    c_TRX = 5,
    c_TRY = 6,
    c_TRZ = 7,
    c_L11L12 = 8,
    c_L13L21 = 9,
    c_L22L23 = 10,
    c_L31L32 = 11,
    c_L33 = 12,
    c_RBK = 13,
    c_GBK = 14,
    c_BBK = 15,
    c_LR1LR2 = 16,
    c_LR3LG1 = 17,
    c_LG2LG3 = 18,
    c_LB1LB2 = 19,
    c_LB3 = 20,
    c_RFC = 21,
    c_GFC = 22,
    c_BFC = 23,
    c_OFX = 24,
    c_OFY = 25,
    c_H = 26,
    c_DQA = 27,
    c_DQB = 28,
    c_ZSF3 = 29,
    c_ZSF4 = 30,
    c_FLAG = 31
};

/* ================================================================
 * FLAG register helpers
 * ================================================================ */
static uint32_t gte_flag;

static inline void flag_reset(void) { gte_flag = 0; }
static inline void flag_set(int bit) { gte_flag |= (1u << bit); }

/* Recompute bit 31 from bits 30-23 and 18-13 */
static inline void flag_update_bit31(void)
{
    if (gte_flag & 0x7F87E000u)
        gte_flag |= 0x80000000u;
}

/* ================================================================
 * Saturation / Clamping helpers
 * ================================================================ */

/* Check MAC overflow (44-bit signed, ie +/-2^43) for MAC1/2/3 */
static inline int64_t check_mac_overflow(int64_t val, int n)
{
    /* n=1,2,3 -> flag bits 30,29,28 (positive), 27,26,25 (negative) */
    if (val > 0x7FFFFFFFFFFll)
        flag_set(30 + 1 - n);
    if (val < -0x80000000000ll)
        flag_set(27 + 1 - n);
    return val;
}

/* Wrap a value to 44-bit signed range (sign-extend from bit 43) */
static inline int64_t wrap44(int64_t val)
{
    return (val << 20) >> 20;
}

/* Check MAC0 overflow (32-bit signed) */
static inline int64_t check_mac0_overflow(int64_t val)
{
    if (val > 0x7FFFFFFFll)
        flag_set(16);
    if (val < -0x80000000ll)
        flag_set(15);
    return val;
}

/* Saturate to signed 16-bit, flag bits 24/23/22 for IR1/2/3 */
static inline int32_t saturate_ir(int64_t val, int n, int lm)
{
    /* n=1,2,3 -> flag bits 24,23,22 */
    /* Flag is set if value is outside clamping range [lo..7FFF] */
    int32_t lo = lm ? 0 : -0x8000;
    if (val < lo || val > 0x7FFF)
        flag_set(24 + 1 - n);
    if (val < lo)
        return lo;
    if (val > 0x7FFF)
        return 0x7FFF;
    return (int32_t)val;
}

/* Special IR saturation for RTPS IR3: flag always checked against -8000..7FFF,
 * but value clamped per lm. */
static inline int32_t saturate_ir_rtps3(int64_t val, int lm)
{
    if (val < -0x8000 || val > 0x7FFF)
        flag_set(22); /* bit 22 = IR3 sat */
    int32_t lo = lm ? 0 : -0x8000;
    if (val < lo)
        return lo;
    if (val > 0x7FFF)
        return 0x7FFF;
    return (int32_t)val;
}

/* Saturate IR0 to 0..+1000h, flag bit 12 */
static inline int32_t saturate_ir0(int64_t val)
{
    if (val < 0)
    {
        flag_set(12);
        return 0;
    }
    if (val > 0x1000)
    {
        flag_set(12);
        return 0x1000;
    }
    return (int32_t)val;
}

/* Saturate SXY to -0400h..+03FFh, flag bits 14/13 */
static inline int32_t saturate_sx(int64_t val)
{
    if (val < -0x400)
    {
        flag_set(14);
        return -0x400;
    }
    if (val > 0x3FF)
    {
        flag_set(14);
        return 0x3FF;
    }
    return (int32_t)val;
}

static inline int32_t saturate_sy(int64_t val)
{
    if (val < -0x400)
    {
        flag_set(13);
        return -0x400;
    }
    if (val > 0x3FF)
    {
        flag_set(13);
        return 0x3FF;
    }
    return (int32_t)val;
}

/* Saturate SZ to 0..+FFFFh, flag bit 18 */
static inline int32_t saturate_sz(int64_t val)
{
    if (val < 0)
    {
        flag_set(18);
        return 0;
    }
    if (val > 0xFFFF)
    {
        flag_set(18);
        return 0xFFFF;
    }
    return (int32_t)val;
}

/* Saturate color to 0..FFh, flag bits 21/20/19 */
static inline uint8_t saturate_color(int ch, int32_t val)
{
    /* ch=0->R(bit21), 1->G(bit20), 2->B(bit19) */
    if (val < 0)
    {
        flag_set(21 - ch);
        return 0;
    }
    if (val > 0xFF)
    {
        flag_set(21 - ch);
        return 0xFF;
    }
    return (uint8_t)val;
}

/* ================================================================
 * Signed 16-bit extraction
 * ================================================================ */
static inline int16_t lo16(uint32_t v) { return (int16_t)(v & 0xFFFF); }
static inline int16_t hi16(uint32_t v) { return (int16_t)(v >> 16); }

/* ================================================================
 * Matrix / Vector accessors
 * ================================================================ */

static int16_t get_matrix(R3000CPU *cpu, int mx, int row, int col)
{
    int base;
    switch (mx)
    {
    case 0:
        base = 0;
        break; /* Rotation (RT) */
    case 1:
        base = 8;
        break; /* Light (L) */
    case 2:
        base = 16;
        break; /* Color (LCM) */
    default:
    {
        /* mx=3: garbage matrix
         * Row 0: -(R<<4), R<<4, IR0  (R = RGBC red byte)
         * Row 1: R13, R13, R13       (R13 = lo16 of ctrl 1)
         * Row 2: R22, R22, R22       (R22 = lo16 of ctrl 2) */
        int idx = row * 3 + col;
        switch (idx)
        {
        case 0:
        {
            int16_t r = (int16_t)(cpu->cp2_data[d_RGBC] & 0xFF);
            return -(r << 4);
        }
        case 1:
        {
            int16_t r = (int16_t)(cpu->cp2_data[d_RGBC] & 0xFF);
            return r << 4;
        }
        case 2:
            return (int16_t)(int32_t)cpu->cp2_data[d_IR0];
        case 3:
            return lo16(cpu->cp2_ctrl[c_RT13RT21]);
        case 4:
            return lo16(cpu->cp2_ctrl[c_RT13RT21]);
        case 5:
            return lo16(cpu->cp2_ctrl[c_RT13RT21]);
        case 6:
            return lo16(cpu->cp2_ctrl[c_RT22RT23]);
        case 7:
            return lo16(cpu->cp2_ctrl[c_RT22RT23]);
        case 8:
            return lo16(cpu->cp2_ctrl[c_RT22RT23]);
        default:
            return 0;
        }
    }
    }
    /* Matrix layout in ctrl regs:
     * row 0: base+0 lo=m11, base+0 hi=m12, base+1 lo=m13
     * row 1: base+1 hi=m21, base+2 lo=m22, base+2 hi=m23
     * row 2: base+3 lo=m31, base+3 hi=m32, base+4 lo=m33
     * linear: i = row*3 + col, reg = base + i/2, lo if i even, hi if i odd */
    int i = row * 3 + col;
    int reg = base + i / 2;
    if (i & 1)
        return hi16(cpu->cp2_ctrl[reg]);
    else
        return lo16(cpu->cp2_ctrl[reg]);
}

/* Get vector element [comp] for vector v (0=V0, 1=V1, 2=V2, 3=IR) */
static int16_t get_vector(R3000CPU *cpu, int v, int comp)
{
    switch (v)
    {
    case 0:
        if (comp == 0)
            return lo16(cpu->cp2_data[d_VXY0]);
        if (comp == 1)
            return hi16(cpu->cp2_data[d_VXY0]);
        return (int16_t)(int32_t)cpu->cp2_data[d_VZ0];
    case 1:
        if (comp == 0)
            return lo16(cpu->cp2_data[d_VXY1]);
        if (comp == 1)
            return hi16(cpu->cp2_data[d_VXY1]);
        return (int16_t)(int32_t)cpu->cp2_data[d_VZ1];
    case 2:
        if (comp == 0)
            return lo16(cpu->cp2_data[d_VXY2]);
        if (comp == 1)
            return hi16(cpu->cp2_data[d_VXY2]);
        return (int16_t)(int32_t)cpu->cp2_data[d_VZ2];
    case 3:
        if (comp == 0)
            return (int16_t)(int32_t)cpu->cp2_data[d_IR1];
        if (comp == 1)
            return (int16_t)(int32_t)cpu->cp2_data[d_IR2];
        return (int16_t)(int32_t)cpu->cp2_data[d_IR3];
    default:
        return 0;
    }
}

/* Get translation vector for cv: 0=TR, 1=BK, 2=FC (bugged), 3=None */
static int32_t get_translation(R3000CPU *cpu, int cv, int comp)
{
    switch (cv)
    {
    case 0:
        return (int32_t)cpu->cp2_ctrl[c_TRX + comp];
    case 1:
        return (int32_t)cpu->cp2_ctrl[c_RBK + comp];
    case 2:
        return (int32_t)cpu->cp2_ctrl[c_RFC + comp];
    case 3:
        return 0;
    default:
        return 0;
    }
}

/* ================================================================
 * Core computation building blocks
 * ================================================================ */

/* Truncate to 44-bit signed (simulate GTE's 44-bit accumulator).
 * Values beyond 44 bits wrap, matching hardware behavior after overflow. */
static inline int64_t trunc44(int64_t val)
{
    return (val << 20) >> 20;
}

/* R5900-safe 64-bit right shift by 12. */
typedef union
{
    int64_t full;
    struct
    {
        uint32_t lo;
        int32_t hi;
    } p;
} split64_t;

static inline int64_t mac_shift(int64_t val, int sf)
{
    if (sf)
        return val >> 12;
    return val;
}

/* Safe multiply: int16_t * int16_t -> int64_t via int32_t intermediate.
 * Avoids 64x64 multiply on R5900. */
static inline int64_t mul16(int16_t a, int16_t b)
{
    return (int64_t)((int32_t)a * (int32_t)b);
}

/* ================================================================
 * GTE Division (UNR-based, for RTPS/RTPT)
 * ================================================================ */
static uint32_t gte_divide(uint16_t h, uint16_t sz3)
{
    if (h < sz3 * 2)
    {
        int z = 0;
        uint16_t d = sz3;
        while (z < 16 && !(d & 0x8000))
        {
            z++;
            d <<= 1;
        }
        uint64_t n = (uint64_t)h << z;
        uint32_t du = d;
        uint32_t u_val = unr_table[((du - 0x7FC0) >> 7)] + 0x101;
        du = ((0x2000080u - (du * u_val)) >> 8);
        du = ((0x0000080u + (du * u_val)) >> 8);
        n = ((n * du) + 0x8000) >> 16;
        if (n > 0x1FFFF)
            n = 0x1FFFF;
        return (uint32_t)n;
    }
    else
    {
        flag_set(17);
        return 0x1FFFF;
    }
}

/* ================================================================
 * Push FIFOs
 * ================================================================ */
static void push_sxy(R3000CPU *cpu, int32_t sx, int32_t sy)
{
    D(d_SXY0) = D(d_SXY1);
    D(d_SXY1) = D(d_SXY2);
    sx = saturate_sx(sx);
    sy = saturate_sy(sy);
    D(d_SXY2) = ((uint32_t)(uint16_t)sx) | ((uint32_t)(uint16_t)sy << 16);
}

static void push_sz(R3000CPU *cpu, int64_t val)
{
    D(d_SZ0) = D(d_SZ1);
    D(d_SZ1) = D(d_SZ2);
    D(d_SZ2) = D(d_SZ3);
    D(d_SZ3) = (uint32_t)saturate_sz(val);
}

static void push_color(R3000CPU *cpu)
{
    D(d_RGB0) = D(d_RGB1);
    D(d_RGB1) = D(d_RGB2);
    uint8_t r = saturate_color(0, (int32_t)D(d_MAC1) >> 4);
    uint8_t g = saturate_color(1, (int32_t)D(d_MAC2) >> 4);
    uint8_t b = saturate_color(2, (int32_t)D(d_MAC3) >> 4);
    uint8_t code = (D(d_RGBC) >> 24) & 0xFF;
    D(d_RGB2) = r | (g << 8) | (b << 16) | (code << 24);
}

/* ================================================================
 * Store MAC1/2/3 and IR1/2/3 from 64-bit accumulators
 * ================================================================ */
static void store_mac_ir(R3000CPU *cpu, int64_t m1, int64_t m2, int64_t m3, int sf, int lm)
{
    check_mac_overflow(m1, 1);
    check_mac_overflow(m2, 2);
    check_mac_overflow(m3, 3);
    D(d_MAC1) = (uint32_t)(int32_t)mac_shift(m1, sf);
    D(d_MAC2) = (uint32_t)(int32_t)mac_shift(m2, sf);
    D(d_MAC3) = (uint32_t)(int32_t)mac_shift(m3, sf);
    D(d_IR1) = (uint32_t)saturate_ir((int32_t)D(d_MAC1), 1, lm);
    D(d_IR2) = (uint32_t)saturate_ir((int32_t)D(d_MAC2), 2, lm);
    D(d_IR3) = (uint32_t)saturate_ir((int32_t)D(d_MAC3), 3, lm);
}

/* ================================================================
 * MVMVA core: Multiply Matrix * Vector + Translation
 * ================================================================ */
static void gte_mvmva(R3000CPU *cpu, int sf, int lm, int mx, int v, int cv)
{
    int16_t vx = get_vector(cpu, v, 0);
    int16_t vy = get_vector(cpu, v, 1);
    int16_t vz = get_vector(cpu, v, 2);

    if (cv == 2)
    {
        /* FC/Bugged: MAC = last 2 multiplication terms only (m_n2*vy + m_n3*vz).
         * The first step (FC<<12 + m_n1*vx) is computed for flag side-effects only.
         * Ref: PCSX-Redux gte.cc MVMVA cv=2 path */
        int64_t fc1 = (int64_t)(int32_t)C(c_RFC) << 12;
        int64_t fc2 = (int64_t)(int32_t)C(c_GFC) << 12;
        int64_t fc3 = (int64_t)(int32_t)C(c_BFC) << 12;

        /* Step 1: MAC stores m_n2*vy + m_n3*vz (last 2 terms) */
        int64_t m1 = (int64_t)get_matrix(cpu, mx, 0, 1) * vy + (int64_t)get_matrix(cpu, mx, 0, 2) * vz;
        int64_t m2 = (int64_t)get_matrix(cpu, mx, 1, 1) * vy + (int64_t)get_matrix(cpu, mx, 1, 2) * vz;
        int64_t m3 = (int64_t)get_matrix(cpu, mx, 2, 1) * vy + (int64_t)get_matrix(cpu, mx, 2, 2) * vz;

        /* Overflow check on the 2-term sums */
        check_mac_overflow(m1, 1);
        check_mac_overflow(m2, 2);
        check_mac_overflow(m3, 3);

        /* Store MAC (shifted, truncated to 32 bits) */
        D(d_MAC1) = (uint32_t)(int32_t)mac_shift(m1, sf);
        D(d_MAC2) = (uint32_t)(int32_t)mac_shift(m2, sf);
        D(d_MAC3) = (uint32_t)(int32_t)mac_shift(m3, sf);

        /* Step 2: Compute FC<<12 + m_n1*vx for flag side effects only */
        int64_t b1 = fc1 + (int64_t)get_matrix(cpu, mx, 0, 0) * vx;
        int64_t b2 = fc2 + (int64_t)get_matrix(cpu, mx, 1, 0) * vx;
        int64_t b3 = fc3 + (int64_t)get_matrix(cpu, mx, 2, 0) * vx;

        check_mac_overflow(b1, 1);
        check_mac_overflow(b2, 2);
        check_mac_overflow(b3, 3);

        /* IR saturation flags from the bugged path (lm=0) */
        saturate_ir((int32_t)mac_shift(b1, sf), 1, 0);
        saturate_ir((int32_t)mac_shift(b2, sf), 2, 0);
        saturate_ir((int32_t)mac_shift(b3, sf), 3, 0);

        /* Actual IR from the stored MAC values (real lm) */
        D(d_IR1) = (uint32_t)saturate_ir((int32_t)D(d_MAC1), 1, lm);
        D(d_IR2) = (uint32_t)saturate_ir((int32_t)D(d_MAC2), 2, lm);
        D(d_IR3) = (uint32_t)saturate_ir((int32_t)D(d_MAC3), 3, lm);
        return;
    }

    int64_t tx1, tx2, tx3;
    if (cv == 3)
    {
        tx1 = tx2 = tx3 = 0;
    }
    else
    {
        tx1 = (int64_t)get_translation(cpu, cv, 0) << 12;
        tx2 = (int64_t)get_translation(cpu, cv, 1) << 12;
        tx3 = (int64_t)get_translation(cpu, cv, 2) << 12;
    }

    /* Hardware uses a 44-bit accumulator that wraps on overflow.
     * After wrapping, subsequent additions can trigger additional overflows.
     * We track both the mathematical 64-bit sum (for the MAC value)
     * and a 44-bit wrapped accumulator (for overflow detection). */
#define MVMVA_STEP(acc_m, acc_hw, prod, ch)     \
    do                                          \
    {                                           \
        acc_m += (prod);                        \
        int64_t _unwrapped = (acc_hw) + (prod); \
        check_mac_overflow(_unwrapped, ch);     \
        acc_hw = wrap44(_unwrapped);            \
    } while (0)

    int64_t m1 = tx1, hw1 = wrap44(tx1);
    check_mac_overflow(tx1, 1);
    MVMVA_STEP(m1, hw1, (int64_t)get_matrix(cpu, mx, 0, 0) * vx, 1);
    MVMVA_STEP(m1, hw1, (int64_t)get_matrix(cpu, mx, 0, 1) * vy, 1);
    MVMVA_STEP(m1, hw1, (int64_t)get_matrix(cpu, mx, 0, 2) * vz, 1);

    int64_t m2 = tx2, hw2 = wrap44(tx2);
    check_mac_overflow(tx2, 2);
    MVMVA_STEP(m2, hw2, (int64_t)get_matrix(cpu, mx, 1, 0) * vx, 2);
    MVMVA_STEP(m2, hw2, (int64_t)get_matrix(cpu, mx, 1, 1) * vy, 2);
    MVMVA_STEP(m2, hw2, (int64_t)get_matrix(cpu, mx, 1, 2) * vz, 2);

    int64_t m3 = tx3, hw3 = wrap44(tx3);
    check_mac_overflow(tx3, 3);
    MVMVA_STEP(m3, hw3, (int64_t)get_matrix(cpu, mx, 2, 0) * vx, 3);
    MVMVA_STEP(m3, hw3, (int64_t)get_matrix(cpu, mx, 2, 1) * vy, 3);
    MVMVA_STEP(m3, hw3, (int64_t)get_matrix(cpu, mx, 2, 2) * vz, 3);

#undef MVMVA_STEP

    store_mac_ir(cpu, m1, m2, m3, sf, lm);
}

/* ================================================================
 * RTPS / RTPT  (Perspective Transformation)
 * ================================================================ */
static void gte_rtps_core(R3000CPU *cpu, int v, int sf, int lm, int last)
{
    int16_t vx = get_vector(cpu, v, 0);
    int16_t vy = get_vector(cpu, v, 1);
    int16_t vz = get_vector(cpu, v, 2);

    int64_t tx = (int64_t)(int32_t)C(c_TRX) << 12;
    int64_t ty = (int64_t)(int32_t)C(c_TRY) << 12;
    int64_t tz = (int64_t)(int32_t)C(c_TRZ) << 12;

    /* Per-step 44-bit accumulator wrapping (same as MVMVA) */
#define RTPS_STEP(acc_m, acc_hw, prod, ch)      \
    do                                          \
    {                                           \
        acc_m += (prod);                        \
        int64_t _unwrapped = (acc_hw) + (prod); \
        check_mac_overflow(_unwrapped, ch);     \
        acc_hw = wrap44(_unwrapped);            \
    } while (0)

    int64_t m1 = tx, hw1 = wrap44(tx);
    check_mac_overflow(tx, 1);
    RTPS_STEP(m1, hw1, mul16(lo16(C(c_RT11RT12)), vx), 1);
    RTPS_STEP(m1, hw1, mul16(hi16(C(c_RT11RT12)), vy), 1);
    RTPS_STEP(m1, hw1, mul16(lo16(C(c_RT13RT21)), vz), 1);

    int64_t m2 = ty, hw2 = wrap44(ty);
    check_mac_overflow(ty, 2);
    RTPS_STEP(m2, hw2, mul16(hi16(C(c_RT13RT21)), vx), 2);
    RTPS_STEP(m2, hw2, mul16(lo16(C(c_RT22RT23)), vy), 2);
    RTPS_STEP(m2, hw2, mul16(hi16(C(c_RT22RT23)), vz), 2);

    int64_t m3 = tz, hw3 = wrap44(tz);
    check_mac_overflow(tz, 3);
    RTPS_STEP(m3, hw3, mul16(lo16(C(c_RT31RT32)), vx), 3);
    RTPS_STEP(m3, hw3, mul16(hi16(C(c_RT31RT32)), vy), 3);
    RTPS_STEP(m3, hw3, mul16(lo16(C(c_RT33)), vz), 3);

#undef RTPS_STEP

    D(d_MAC1) = (uint32_t)(int32_t)mac_shift(m1, sf);
    D(d_MAC2) = (uint32_t)(int32_t)mac_shift(m2, sf);
    D(d_MAC3) = (uint32_t)(int32_t)mac_shift(m3, sf);

    /* IR1/IR2 saturated. Flag always set per lm range, value clamped per lm. */
    D(d_IR1) = (uint32_t)saturate_ir((int32_t)D(d_MAC1), 1, lm);
    D(d_IR2) = (uint32_t)saturate_ir((int32_t)D(d_MAC2), 2, lm);

    /* IR3 in RTPS: special handling per Lm_B3_sf from reference.
     * FLAG bit 22 is checked against (raw_mac3 >> 12), always.
     * VALUE is clamped from (raw_mac3 >> sf*12), i.e. same shift as MAC3. */
    {
        int64_t val_12 = m3 >> 12; /* always >> 12 for flag check */
        if (val_12 < -0x8000 || val_12 > 0x7FFF)
            flag_set(22);
        int32_t val_sf = (int32_t)mac_shift(m3, sf);
        int32_t lo = lm ? 0 : -0x8000;
        if (val_sf < lo)
            D(d_IR3) = (uint32_t)lo;
        else if (val_sf > 0x7FFF)
            D(d_IR3) = 0x7FFF;
        else
            D(d_IR3) = (uint32_t)val_sf;
    }

    /* Push SZ FIFO: SZ3 from 44-bit truncated accumulator >> 12 */
    push_sz(cpu, trunc44(m3) >> 12);

    /* Perspective division using UNR */
    uint32_t div_result = gte_divide((uint16_t)(int16_t)(int32_t)C(c_H), D(d_SZ3));

    /* SX2 = MAC0/10000h, SY2 = MAC0/10000h */
    int64_t sx_mac = (int64_t)(int32_t)div_result * (int16_t)(int32_t)D(d_IR1) + (int32_t)C(c_OFX);
    int64_t sy_mac = (int64_t)(int32_t)div_result * (int16_t)(int32_t)D(d_IR2) + (int32_t)C(c_OFY);

    check_mac0_overflow(sx_mac);
    check_mac0_overflow(sy_mac);

    push_sxy(cpu, (int32_t)(sx_mac >> 16), (int32_t)(sy_mac >> 16));

    if (last)
    {
        /* Depth cueing: MAC0 = DQA * div_result + DQB */
        int64_t dq_mac = (int64_t)(int16_t)(int32_t)C(c_DQA) * (int32_t)div_result + (int32_t)C(c_DQB);
        check_mac0_overflow(dq_mac);
        D(d_MAC0) = (uint32_t)(int32_t)dq_mac;
        D(d_IR0) = (uint32_t)saturate_ir0(dq_mac >> 12);
    }
}

static void gte_cmd_rtps(R3000CPU *cpu, int sf, int lm)
{
    gte_rtps_core(cpu, 0, sf, lm, 1);
}

static void gte_cmd_rtpt(R3000CPU *cpu, int sf, int lm)
{
    gte_rtps_core(cpu, 0, sf, lm, 0);
    gte_rtps_core(cpu, 1, sf, lm, 0);
    gte_rtps_core(cpu, 2, sf, lm, 1);
}

/* ================================================================
 * NCLIP  (Normal Clipping)
 * ================================================================ */
static void gte_cmd_nclip(R3000CPU *cpu)
{
    int16_t sx0 = lo16(D(d_SXY0)), sy0 = hi16(D(d_SXY0));
    int16_t sx1 = lo16(D(d_SXY1)), sy1 = hi16(D(d_SXY1));
    int16_t sx2 = lo16(D(d_SXY2)), sy2 = hi16(D(d_SXY2));

    int64_t val = (int64_t)sx0 * (sy1 - sy2) +
                  (int64_t)sx1 * (sy2 - sy0) +
                  (int64_t)sx2 * (sy0 - sy1);
    check_mac0_overflow(val);
    D(d_MAC0) = (uint32_t)(int32_t)val;
}

/* ================================================================
 * AVSZ3 / AVSZ4
 * ================================================================ */
static void gte_cmd_avsz3(R3000CPU *cpu)
{
    int64_t val = (int64_t)(int16_t)(int32_t)C(c_ZSF3) * ((int64_t)D(d_SZ1) + D(d_SZ2) + D(d_SZ3));
    check_mac0_overflow(val);
    D(d_MAC0) = (uint32_t)(int32_t)val;
    D(d_OTZ) = (uint32_t)saturate_sz(val >> 12);
}

static void gte_cmd_avsz4(R3000CPU *cpu)
{
    int64_t val = (int64_t)(int16_t)(int32_t)C(c_ZSF4) * ((int64_t)D(d_SZ0) + D(d_SZ1) + D(d_SZ2) + D(d_SZ3));
    check_mac0_overflow(val);
    D(d_MAC0) = (uint32_t)(int32_t)val;
    D(d_OTZ) = (uint32_t)saturate_sz(val >> 12);
}

/* ================================================================
 * OP(sf, lm) - Cross product of 2 vectors
 * ================================================================ */
static void gte_cmd_op(R3000CPU *cpu, int sf, int lm)
{
    int16_t d1 = lo16(C(c_RT11RT12));
    int16_t d2 = lo16(C(c_RT22RT23));
    int16_t d3 = lo16(C(c_RT33));
    int16_t ir1 = (int16_t)(int32_t)D(d_IR1);
    int16_t ir2 = (int16_t)(int32_t)D(d_IR2);
    int16_t ir3 = (int16_t)(int32_t)D(d_IR3);

    int64_t m1 = (int64_t)ir3 * d2 - (int64_t)ir2 * d3;
    int64_t m2 = (int64_t)ir1 * d3 - (int64_t)ir3 * d1;
    int64_t m3 = (int64_t)ir2 * d1 - (int64_t)ir1 * d2;

    store_mac_ir(cpu, m1, m2, m3, sf, lm);
}

/* ================================================================
 * MVMVA command wrapper
 * ================================================================ */
static void gte_cmd_mvmva(R3000CPU *cpu, int sf, int lm, int mx, int v, int cv)
{
    gte_mvmva(cpu, sf, lm, mx, v, cv);
}

/* ================================================================
 * SQR(sf) - Square of vector IR
 * ================================================================ */
static void gte_cmd_sqr(R3000CPU *cpu, int sf, int lm)
{
    int16_t ir1 = (int16_t)(int32_t)D(d_IR1);
    int16_t ir2 = (int16_t)(int32_t)D(d_IR2);
    int16_t ir3 = (int16_t)(int32_t)D(d_IR3);

    int64_t m1 = (int64_t)ir1 * ir1;
    int64_t m2 = (int64_t)ir2 * ir2;
    int64_t m3 = (int64_t)ir3 * ir3;

    store_mac_ir(cpu, m1, m2, m3, sf, lm);
}

/* ================================================================
 * Color calculation helpers
 * ================================================================ */

/* Interpolate: result = acc + (FC - acc) * IR0
 * acc1/acc2/acc3 are the RAW accumulator values (before sf-shift). */
static void interpolate_color_acc(R3000CPU *cpu, int64_t acc1, int64_t acc2, int64_t acc3, int sf, int lm)
{
    int64_t fc1 = (int64_t)(int32_t)C(c_RFC) << 12;
    int64_t fc2 = (int64_t)(int32_t)C(c_GFC) << 12;
    int64_t fc3 = (int64_t)(int32_t)C(c_BFC) << 12;

    int64_t d1 = fc1 - acc1;
    int64_t d2 = fc2 - acc2;
    int64_t d3 = fc3 - acc3;

    check_mac_overflow(d1, 1);
    check_mac_overflow(d2, 2);
    check_mac_overflow(d3, 3);

    /* Saturate to -8000h..+7FFFh (lm=0 for this intermediate step).
     * The hardware stores (shifted >> sf*12) in 32-bit MAC first,
     * then derives IR from that truncated 32-bit value. */
    int32_t tmp_ir1 = saturate_ir((int32_t)mac_shift(d1, sf), 1, 0);
    int32_t tmp_ir2 = saturate_ir((int32_t)mac_shift(d2, sf), 2, 0);
    int32_t tmp_ir3 = saturate_ir((int32_t)mac_shift(d3, sf), 3, 0);

    int16_t ir0 = (int16_t)(int32_t)D(d_IR0);

    /* [MAC1,MAC2,MAC3] = ([IR1,IR2,IR3] * IR0) + accumulator.
     * Use safe 32-bit multiply since tmp_ir and ir0 fit in int16_t range. */
    int64_t r1 = (int64_t)((int32_t)tmp_ir1 * (int32_t)ir0) + acc1;
    int64_t r2 = (int64_t)((int32_t)tmp_ir2 * (int32_t)ir0) + acc2;
    int64_t r3 = (int64_t)((int32_t)tmp_ir3 * (int32_t)ir0) + acc3;

    store_mac_ir(cpu, r1, r2, r3, sf, lm);
}

/* ================================================================
 * NCS / NCT  - Normal Color
 * ================================================================ */
static void gte_ncs_core(R3000CPU *cpu, int v, int sf, int lm)
{
    /* Step 1: Light matrix * V */
    gte_mvmva(cpu, sf, lm, 1, v, 3);

    /* Step 2: BK + LCM * IR */
    gte_mvmva(cpu, sf, lm, 2, 3, 1);

    /* NCS: No R*IR step. Push color directly from step 2 result. */
    push_color(cpu);
}

static void gte_cmd_ncs(R3000CPU *cpu, int sf, int lm)
{
    gte_ncs_core(cpu, 0, sf, lm);
}

static void gte_cmd_nct(R3000CPU *cpu, int sf, int lm)
{
    gte_ncs_core(cpu, 0, sf, lm);
    gte_ncs_core(cpu, 1, sf, lm);
    gte_ncs_core(cpu, 2, sf, lm);
}

/* ================================================================
 * NCCS / NCCT  - Normal Color Color
 * ================================================================ */
static void gte_nccs_core(R3000CPU *cpu, int v, int sf, int lm)
{
    gte_mvmva(cpu, sf, lm, 1, v, 3);
    gte_mvmva(cpu, sf, lm, 2, 3, 1);

    uint8_t r = D(d_RGBC) & 0xFF;
    uint8_t g = (D(d_RGBC) >> 8) & 0xFF;
    uint8_t b = (D(d_RGBC) >> 16) & 0xFF;

    int64_t m1 = ((int64_t)r * (int16_t)(int32_t)D(d_IR1)) << 4;
    int64_t m2 = ((int64_t)g * (int16_t)(int32_t)D(d_IR2)) << 4;
    int64_t m3 = ((int64_t)b * (int16_t)(int32_t)D(d_IR3)) << 4;

    store_mac_ir(cpu, m1, m2, m3, sf, lm);
    push_color(cpu);
}

static void gte_cmd_nccs(R3000CPU *cpu, int sf, int lm)
{
    gte_nccs_core(cpu, 0, sf, lm);
}

static void gte_cmd_ncct(R3000CPU *cpu, int sf, int lm)
{
    gte_nccs_core(cpu, 0, sf, lm);
    gte_nccs_core(cpu, 1, sf, lm);
    gte_nccs_core(cpu, 2, sf, lm);
}

/* ================================================================
 * NCDS / NCDT - Normal Color Depth Cue
 * ================================================================ */
static void gte_ncds_core(R3000CPU *cpu, int v, int sf, int lm)
{
    gte_mvmva(cpu, sf, lm, 1, v, 3);
    gte_mvmva(cpu, sf, lm, 2, 3, 1);

    uint8_t r = D(d_RGBC) & 0xFF;
    uint8_t g = (D(d_RGBC) >> 8) & 0xFF;
    uint8_t b = (D(d_RGBC) >> 16) & 0xFF;

    int64_t m1 = ((int64_t)r * (int16_t)(int32_t)D(d_IR1)) << 4;
    int64_t m2 = ((int64_t)g * (int16_t)(int32_t)D(d_IR2)) << 4;
    int64_t m3 = ((int64_t)b * (int16_t)(int32_t)D(d_IR3)) << 4;

    store_mac_ir(cpu, m1, m2, m3, sf, lm);
    interpolate_color_acc(cpu, m1, m2, m3, sf, lm);
    push_color(cpu);
}

static void gte_cmd_ncds(R3000CPU *cpu, int sf, int lm)
{
    gte_ncds_core(cpu, 0, sf, lm);
}

static void gte_cmd_ncdt(R3000CPU *cpu, int sf, int lm)
{
    gte_ncds_core(cpu, 0, sf, lm);
    gte_ncds_core(cpu, 1, sf, lm);
    gte_ncds_core(cpu, 2, sf, lm);
}

/* ================================================================
 * CC - Color Color
 * ================================================================ */
static void gte_cmd_cc(R3000CPU *cpu, int sf, int lm)
{
    gte_mvmva(cpu, sf, lm, 2, 3, 1);

    uint8_t r = D(d_RGBC) & 0xFF;
    uint8_t g = (D(d_RGBC) >> 8) & 0xFF;
    uint8_t b = (D(d_RGBC) >> 16) & 0xFF;

    int64_t m1 = ((int64_t)r * (int16_t)(int32_t)D(d_IR1)) << 4;
    int64_t m2 = ((int64_t)g * (int16_t)(int32_t)D(d_IR2)) << 4;
    int64_t m3 = ((int64_t)b * (int16_t)(int32_t)D(d_IR3)) << 4;

    store_mac_ir(cpu, m1, m2, m3, sf, lm);
    push_color(cpu);
}

/* ================================================================
 * CDP - Color Depth Cue
 * ================================================================ */
static void gte_cmd_cdp(R3000CPU *cpu, int sf, int lm)
{
    gte_mvmva(cpu, sf, lm, 2, 3, 1);

    uint8_t r = D(d_RGBC) & 0xFF;
    uint8_t g = (D(d_RGBC) >> 8) & 0xFF;
    uint8_t b = (D(d_RGBC) >> 16) & 0xFF;

    int64_t m1 = ((int64_t)r * (int16_t)(int32_t)D(d_IR1)) << 4;
    int64_t m2 = ((int64_t)g * (int16_t)(int32_t)D(d_IR2)) << 4;
    int64_t m3 = ((int64_t)b * (int16_t)(int32_t)D(d_IR3)) << 4;

    store_mac_ir(cpu, m1, m2, m3, sf, lm);
    interpolate_color_acc(cpu, m1, m2, m3, sf, lm);
    push_color(cpu);
}

/* ================================================================
 * DPCS - Depth Cueing (single)
 * ================================================================ */
static void gte_cmd_dpcs(R3000CPU *cpu, int sf, int lm)
{
    uint8_t r = D(d_RGBC) & 0xFF;
    uint8_t g = (D(d_RGBC) >> 8) & 0xFF;
    uint8_t b = (D(d_RGBC) >> 16) & 0xFF;

    /* Accumulator = [R,G,B] << 16 */
    int64_t m1 = (int64_t)r << 16;
    int64_t m2 = (int64_t)g << 16;
    int64_t m3 = (int64_t)b << 16;

    /* Overflow check (these never overflow, but set flags if needed) */
    check_mac_overflow(m1, 1);
    check_mac_overflow(m2, 2);
    check_mac_overflow(m3, 3);

    /* Interpolate directly from accumulator values */
    interpolate_color_acc(cpu, m1, m2, m3, sf, lm);
    push_color(cpu);
}

/* ================================================================
 * DPCT - Depth Cueing (triple)
 * ================================================================ */
static void gte_cmd_dpct(R3000CPU *cpu, int sf, int lm)
{
    int i;
    for (i = 0; i < 3; i++)
    {
        uint8_t r = D(d_RGB0) & 0xFF;
        uint8_t g = (D(d_RGB0) >> 8) & 0xFF;
        uint8_t b = (D(d_RGB0) >> 16) & 0xFF;

        int64_t m1 = (int64_t)r << 16;
        int64_t m2 = (int64_t)g << 16;
        int64_t m3 = (int64_t)b << 16;

        check_mac_overflow(m1, 1);
        check_mac_overflow(m2, 2);
        check_mac_overflow(m3, 3);

        interpolate_color_acc(cpu, m1, m2, m3, sf, lm);
        push_color(cpu);
    }
}

/* ================================================================
 * INTPL - Interpolation of a vector and far color
 * ================================================================ */
static void gte_cmd_intpl(R3000CPU *cpu, int sf, int lm)
{
    int64_t m1 = (int64_t)(int16_t)(int32_t)D(d_IR1) << 12;
    int64_t m2 = (int64_t)(int16_t)(int32_t)D(d_IR2) << 12;
    int64_t m3 = (int64_t)(int16_t)(int32_t)D(d_IR3) << 12;

    store_mac_ir(cpu, m1, m2, m3, sf, lm);
    interpolate_color_acc(cpu, m1, m2, m3, sf, lm);
    push_color(cpu);
}

/* ================================================================
 * DCPL - Depth Cue Color Light
 * ================================================================ */
static void gte_cmd_dcpl(R3000CPU *cpu, int sf, int lm)
{
    uint8_t r = D(d_RGBC) & 0xFF;
    uint8_t g = (D(d_RGBC) >> 8) & 0xFF;
    uint8_t b = (D(d_RGBC) >> 16) & 0xFF;

    int64_t m1 = ((int64_t)r * (int16_t)(int32_t)D(d_IR1)) << 4;
    int64_t m2 = ((int64_t)g * (int16_t)(int32_t)D(d_IR2)) << 4;
    int64_t m3 = ((int64_t)b * (int16_t)(int32_t)D(d_IR3)) << 4;

    store_mac_ir(cpu, m1, m2, m3, sf, lm);
    interpolate_color_acc(cpu, m1, m2, m3, sf, lm);
    push_color(cpu);
}

/* ================================================================
 * GPF(sf, lm) - General Purpose Interpolation
 * ================================================================ */
static void gte_cmd_gpf(R3000CPU *cpu, int sf, int lm)
{
    int16_t ir0 = (int16_t)(int32_t)D(d_IR0);
    int16_t ir1 = (int16_t)(int32_t)D(d_IR1);
    int16_t ir2 = (int16_t)(int32_t)D(d_IR2);
    int16_t ir3 = (int16_t)(int32_t)D(d_IR3);

    int64_t m1 = (int64_t)ir1 * ir0;
    int64_t m2 = (int64_t)ir2 * ir0;
    int64_t m3 = (int64_t)ir3 * ir0;

    store_mac_ir(cpu, m1, m2, m3, sf, lm);
    push_color(cpu);
}

/* ================================================================
 * GPL(sf, lm) - General Purpose Interpolation with base
 * ================================================================ */
static void gte_cmd_gpl(R3000CPU *cpu, int sf, int lm)
{
    int16_t ir0 = (int16_t)(int32_t)D(d_IR0);
    int16_t ir1 = (int16_t)(int32_t)D(d_IR1);
    int16_t ir2 = (int16_t)(int32_t)D(d_IR2);
    int16_t ir3 = (int16_t)(int32_t)D(d_IR3);

    int64_t mac1 = (int64_t)(int32_t)D(d_MAC1);
    int64_t mac2 = (int64_t)(int32_t)D(d_MAC2);
    int64_t mac3 = (int64_t)(int32_t)D(d_MAC3);

    if (sf)
    {
        mac1 <<= 12;
        mac2 <<= 12;
        mac3 <<= 12;
    }

    int64_t m1 = (int64_t)ir1 * ir0 + mac1;
    int64_t m2 = (int64_t)ir2 * ir0 + mac2;
    int64_t m3 = (int64_t)ir3 * ir0 + mac3;

    store_mac_ir(cpu, m1, m2, m3, sf, lm);
    push_color(cpu);
}

/* ================================================================
 * GTE Register Read/Write
 * ================================================================ */

static uint32_t gte_count_leading(uint32_t val)
{
    if (val == 0)
        return 32;
    uint32_t target;
    if ((int32_t)val < 0)
    {
        target = ~val;
        if (target == 0)
            return 32;
    }
    else
    {
        target = val;
    }
    uint32_t count = 0;
    while (!(target & 0x80000000))
    {
        count++;
        target <<= 1;
    }
    return count;
}

uint32_t GTE_ReadData(R3000CPU *cpu, int reg)
{
    switch (reg)
    {
    case 15:
        return D(14);
    case 28:
    case 29:
    {
        /* Clamp IR/0x80 to 0..0x1F (saturate, not mask) */
        int32_t rv = (int32_t)D(9) >> 7;
        int32_t gv = (int32_t)D(10) >> 7;
        int32_t bv = (int32_t)D(11) >> 7;
        if (rv < 0)
            rv = 0;
        if (rv > 0x1F)
            rv = 0x1F;
        if (gv < 0)
            gv = 0;
        if (gv > 0x1F)
            gv = 0x1F;
        if (bv < 0)
            bv = 0;
        if (bv > 0x1F)
            bv = 0x1F;
        return (uint32_t)rv | ((uint32_t)gv << 5) | ((uint32_t)bv << 10);
    }
    default:
        return D(reg & 0x1F);
    }
}

void GTE_WriteData(R3000CPU *cpu, int reg, uint32_t val)
{
    switch (reg)
    {
    case 1:
    case 3:
    case 5:
    case 8:
    case 9:
    case 10:
    case 11:
        D(reg) = (uint32_t)(int32_t)(int16_t)(val & 0xFFFF);
        break;
    case 7:
    case 16:
    case 17:
    case 18:
    case 19:
        D(reg) = val & 0xFFFF;
        break;
    case 15:
        D(12) = D(13);
        D(13) = D(14);
        D(14) = val;
        D(15) = val;
        break;
    case 28:
        D(9) = (uint32_t)(int32_t)(int16_t)(((val >> 0) & 0x1F) << 7);
        D(10) = (uint32_t)(int32_t)(int16_t)(((val >> 5) & 0x1F) << 7);
        D(11) = (uint32_t)(int32_t)(int16_t)(((val >> 10) & 0x1F) << 7);
        break;
    case 29:
        break;
    case 30:
        D(30) = val;
        D(31) = gte_count_leading(val);
        break;
    case 31:
        break;
    default:
        D(reg & 0x1F) = val;
        break;
    }
}

uint32_t GTE_ReadCtrl(R3000CPU *cpu, int reg)
{
    return C(reg & 0x1F);
}

void GTE_WriteCtrl(R3000CPU *cpu, int reg, uint32_t val)
{
    switch (reg)
    {
    case 4:
    case 12:
    case 20:
    case 26:
    case 27:
    case 29:
    case 30:
        C(reg) = (uint32_t)(int32_t)(int16_t)(val & 0xFFFF);
        break;
    case 31:
    {
        uint32_t flag = val & 0x7FFFF000;
        flag |= (flag & 0x7F87E000) ? 0x80000000 : 0;
        C(31) = flag;
        break;
    }
    default:
        C(reg & 0x1F) = val;
        break;
    }
}

/* ================================================================
 * Main GTE Command Dispatcher
 * ================================================================ */
void GTE_Execute(uint32_t opcode, R3000CPU *cpu)
{
    uint32_t func = opcode & 0x3F;
    int sf = (opcode >> 19) & 1;
    int lm = (opcode >> 10) & 1;
    int mx = (opcode >> 17) & 3;
    int v = (opcode >> 15) & 3;
    int cv = (opcode >> 13) & 3;

    /* Clear FLAG at start of each command */
    flag_reset();

    switch (func)
    {
    case 0x01:
        gte_cmd_rtps(cpu, sf, lm);
        break;
    case 0x06:
        gte_cmd_nclip(cpu);
        break;
    case 0x0C:
        gte_cmd_op(cpu, sf, lm);
        break;
    case 0x10:
        gte_cmd_dpcs(cpu, sf, lm);
        break;
    case 0x11:
        gte_cmd_intpl(cpu, sf, lm);
        break;
    case 0x12:
        gte_cmd_mvmva(cpu, sf, lm, mx, v, cv);
        break;
    case 0x13:
        gte_cmd_ncds(cpu, sf, lm);
        break;
    case 0x14:
        gte_cmd_cdp(cpu, sf, lm);
        break;
    case 0x16:
        gte_cmd_ncdt(cpu, sf, lm);
        break;
    case 0x1B:
        gte_cmd_nccs(cpu, sf, lm);
        break;
    case 0x1C:
        gte_cmd_cc(cpu, sf, lm);
        break;
    case 0x1E:
        gte_cmd_ncs(cpu, sf, lm);
        break;
    case 0x20:
        gte_cmd_nct(cpu, sf, lm);
        break;
    case 0x28:
        gte_cmd_sqr(cpu, sf, lm);
        break;
    case 0x29:
        gte_cmd_dcpl(cpu, sf, lm);
        break;
    case 0x2A:
        gte_cmd_dpct(cpu, sf, lm);
        break;
    case 0x2D:
        gte_cmd_avsz3(cpu);
        break;
    case 0x2E:
        gte_cmd_avsz4(cpu);
        break;
    case 0x30:
        gte_cmd_rtpt(cpu, sf, lm);
        break;
    case 0x3D:
        gte_cmd_gpf(cpu, sf, lm);
        break;
    case 0x3E:
        gte_cmd_gpl(cpu, sf, lm);
        break;
    case 0x3F:
        gte_cmd_ncct(cpu, sf, lm);
        break;
    default:
        break;
    }

    /* Update FLAG register */
    flag_update_bit31();
    C(c_FLAG) = gte_flag;
}
