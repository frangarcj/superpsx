#include "superpsx.h"
#include <stdio.h>

/* GTE Register Indices (Data) */
#define GTE_V0_XY 0x00
#define GTE_V0_Z  0x01
#define GTE_V1_XY 0x02
#define GTE_V1_Z  0x03
#define GTE_V2_XY 0x04
#define GTE_V2_Z  0x05
#define GTE_RGBC  0x06
#define GTE_OTZ   0x07
#define GTE_IR0   0x08
#define GTE_IR1   0x09
#define GTE_IR2   0x0A
#define GTE_IR3   0x0B
#define GTE_SXY0  0x0C
#define GTE_SXY1  0x0D
#define GTE_SXY2  0x0E
#define GTE_SXYP  0x0F
#define GTE_SZ0   0x10
#define GTE_SZ1   0x11
#define GTE_SZ2   0x12
#define GTE_SZ3   0x13
#define GTE_RGB0  0x14
#define GTE_RGB1  0x15
#define GTE_RGB2  0x16
#define GTE_RES1  0x17
#define GTE_MAC0  0x18
#define GTE_MAC1  0x19
#define GTE_MAC2  0x1A
#define GTE_MAC3  0x1B
#define GTE_IRGB  0x1C
#define GTE_ORGB  0x1D
#define GTE_LZCS  0x1E
#define GTE_LZCR  0x1F

/* GTE Register Indices (Control) */
#define GTE_R11_R12 0x00
#define GTE_R13_R21 0x01
#define GTE_R22_R23 0x02
#define GTE_R31_R32 0x03
#define GTE_R33     0x04
#define GTE_TRX     0x05
#define GTE_TRY     0x06
#define GTE_TRZ     0x07
#define GTE_L11_L12 0x08
#define GTE_L13_L21 0x09
#define GTE_L22_L23 0x0A
#define GTE_L31_L32 0x0B
#define GTE_L33     0x0C
#define GTE_RBK     0x0D
#define GTE_GBK     0x0E
#define GTE_BBK     0x0F
#define GTE_LR1_LR2 0x10
#define GTE_LR3_LG1 0x11
#define GTE_LG2_LG3 0x12
#define GTE_LB1_LB2 0x13
#define GTE_LB3     0x14
#define GTE_RFC     0x15
#define GTE_GFC     0x16
#define GTE_BFC     0x17
#define GTE_OFX     0x18
#define GTE_OFY     0x19
#define GTE_H       0x1A
#define GTE_DQA     0x1B
#define GTE_DQB     0x1C
#define GTE_ZSF3    0x1D
#define GTE_ZSF4    0x1E
#define GTE_FLAG    0x1F

/* Signed 16-bit access helpers */
static s16 S16(u32 val) { return (s16)(val & 0xFFFF); }
static s32 S32(u32 val) { return (s32)val; } // Usually 32-bit in reg

/* ---- GTE Register Read/Write Helpers ---- */

/* Count leading zeros or leading ones (for LZCS/LZCR) */
static u32 gte_count_leading(u32 val) {
    if (val == 0) return 32;
    u32 target;
    if ((s32)val < 0) {
        target = ~val;
        if (target == 0) return 32;
    } else {
        target = val;
    }
    u32 count = 0;
    while (!(target & 0x80000000)) {
        count++;
        target <<= 1;
    }
    return count;
}

/* Read GTE Data Register (for MFC2 / SWC2) */
u32 GTE_ReadData(R3000CPU *cpu, int reg) {
    switch (reg) {
        case 15: /* SXYP reads as SXY2 */
            return cpu->cp2_data[14];
        case 28: /* IRGB - computed from IR1/2/3 */
        case 29: /* ORGB - same as IRGB read */
        {
            u32 r = ((s32)cpu->cp2_data[9] >> 7) & 0x1F;
            u32 g = ((s32)cpu->cp2_data[10] >> 7) & 0x1F;
            u32 b = ((s32)cpu->cp2_data[11] >> 7) & 0x1F;
            /* Clamp negative IR values to 0 */
            if ((s32)cpu->cp2_data[9] < 0) r = 0;
            if ((s32)cpu->cp2_data[10] < 0) g = 0;
            if ((s32)cpu->cp2_data[11] < 0) b = 0;
            return r | (g << 5) | (b << 10);
        }
        default:
            return cpu->cp2_data[reg & 0x1F];
    }
}

/* Write GTE Data Register (for MTC2 / LWC2) */
void GTE_WriteData(R3000CPU *cpu, int reg, u32 val) {
    switch (reg) {
        case 1:  /* V0_Z - sign-extend 16-bit */
        case 3:  /* V1_Z */
        case 5:  /* V2_Z */
        case 8:  /* IR0 */
        case 9:  /* IR1 */
        case 10: /* IR2 */
        case 11: /* IR3 */
            cpu->cp2_data[reg] = (u32)(s32)(s16)(val & 0xFFFF);
            break;
        case 7:  /* OTZ - zero-extend 16-bit */
        case 16: /* SZ0 */
        case 17: /* SZ1 */
        case 18: /* SZ2 */
        case 19: /* SZ3 */
            cpu->cp2_data[reg] = val & 0xFFFF;
            break;
        case 15: /* SXYP - push SXY FIFO */
            cpu->cp2_data[12] = cpu->cp2_data[13]; /* SXY0 <- SXY1 */
            cpu->cp2_data[13] = cpu->cp2_data[14]; /* SXY1 <- SXY2 */
            cpu->cp2_data[14] = val;                /* SXY2 <- new */
            cpu->cp2_data[15] = val;                /* also store in SXYP slot */
            break;
        case 28: /* IRGB - sets IR1/IR2/IR3 from 5-bit color fields */
            cpu->cp2_data[9]  = (u32)(s32)(s16)(((val >>  0) & 0x1F) << 7); /* IR1 */
            cpu->cp2_data[10] = (u32)(s32)(s16)(((val >>  5) & 0x1F) << 7); /* IR2 */
            cpu->cp2_data[11] = (u32)(s32)(s16)(((val >> 10) & 0x1F) << 7); /* IR3 */
            /* Note: the raw IRGB value is NOT stored; reads are computed from IR1/2/3 */
            break;
        case 29: /* ORGB - read-only, writes are ignored */
            break;
        case 30: /* LZCS - write stores value and computes LZCR */
            cpu->cp2_data[30] = val;
            cpu->cp2_data[31] = gte_count_leading(val);
            break;
        case 31: /* LZCR - read-only, writes are ignored */
            break;
        default:
            cpu->cp2_data[reg & 0x1F] = val;
            break;
    }
}

/* Read GTE Control Register (for CFC2) */
u32 GTE_ReadCtrl(R3000CPU *cpu, int reg) {
    return cpu->cp2_ctrl[reg & 0x1F];
}

/* Write GTE Control Register (for CTC2) */
void GTE_WriteCtrl(R3000CPU *cpu, int reg, u32 val) {
    switch (reg) {
        case 4:  /* R33 - sign-extend 16-bit */
        case 12: /* L33 */
        case 20: /* LB3 */
        case 26: /* H */
        case 27: /* DQA */
        case 29: /* ZSF3 */
        case 30: /* ZSF4 */
            cpu->cp2_ctrl[reg] = (u32)(s32)(s16)(val & 0xFFFF);
            break;
        case 31: /* FLAG - clear bits 0-11, recompute bit 31 */
        {
            u32 flag = val & 0x7FFFF000; /* clear bits 0-11 and bit 31 */
            /* bit31 = OR of bits 30-23 and bits 18-13 */
            flag |= (flag & 0x7F87E000) ? 0x80000000 : 0;
            cpu->cp2_ctrl[31] = flag;
            break;
        }
        default:
            cpu->cp2_ctrl[reg & 0x1F] = val;
            break;
    }
}

/* GTE Context Helper */
static s64 MAC0, MAC1, MAC2, MAC3;

static void GTE_ProcessVertex(R3000CPU *cpu, int v_idx) {
    // Input Vector
    s16 vx, vy, vz;
    if (v_idx == 0) {
        vx = S16(cpu->cp2_data[GTE_V0_XY]);
        vy = S16(cpu->cp2_data[GTE_V0_XY] >> 16);
        vz = S16(cpu->cp2_data[GTE_V0_Z]);
    } else if (v_idx == 1) {
        vx = S16(cpu->cp2_data[GTE_V1_XY]);
        vy = S16(cpu->cp2_data[GTE_V1_XY] >> 16);
        vz = S16(cpu->cp2_data[GTE_V1_Z]);
    } else {
        vx = S16(cpu->cp2_data[GTE_V2_XY]);
        vy = S16(cpu->cp2_data[GTE_V2_XY] >> 16);
        vz = S16(cpu->cp2_data[GTE_V2_Z]);
    }

    // Rotation Matrix
    s16 r11 = S16(cpu->cp2_ctrl[GTE_R11_R12]);
    s16 r12 = S16(cpu->cp2_ctrl[GTE_R11_R12] >> 16);
    s16 r13 = S16(cpu->cp2_ctrl[GTE_R13_R21]);
    s16 r21 = S16(cpu->cp2_ctrl[GTE_R13_R21] >> 16);
    s16 r22 = S16(cpu->cp2_ctrl[GTE_R22_R23]);
    s16 r23 = S16(cpu->cp2_ctrl[GTE_R22_R23] >> 16);
    s16 r31 = S16(cpu->cp2_ctrl[GTE_R31_R32]);
    s16 r32 = S16(cpu->cp2_ctrl[GTE_R31_R32] >> 16);
    s16 r33 = S16(cpu->cp2_ctrl[GTE_R33]);

    // Translation Vector
    s32 trx = S32(cpu->cp2_ctrl[GTE_TRX]);
    s32 try = S32(cpu->cp2_ctrl[GTE_TRY]);
    s32 trz = S32(cpu->cp2_ctrl[GTE_TRZ]);

    // Rotate and Translate
    s64 mx = ((s64)trx << 12) + (r11 * vx) + (r12 * vy) + (r13 * vz);
    s64 my = ((s64)try << 12) + (r21 * vx) + (r22 * vy) + (r23 * vz);
    s64 mz = ((s64)trz << 12) + (r31 * vx) + (r32 * vy) + (r33 * vz);

    // IR1, IR2, IR3 = MAC >> 12
    s16 ir1 = (mx >> 12);
    s16 ir2 = (my >> 12);
    s16 ir3 = (mz >> 12);
    
    // Store Intermediate results
    cpu->cp2_data[GTE_IR1] = ir1;
    cpu->cp2_data[GTE_IR2] = ir2;
    cpu->cp2_data[GTE_IR3] = ir3;

    // Perspective Projection
    u16 h = (u16)cpu->cp2_ctrl[GTE_H];
    s32 ofx = S32(cpu->cp2_ctrl[GTE_OFX]);
    s32 ofy = S32(cpu->cp2_ctrl[GTE_OFY]);
    
    // Avoid div by zero
    s32 z = ir3;
    if (z == 0) z = 1; 
    
    s32 sx = (ir1 * h / z) + ofx;
    s32 sy = (ir2 * h / z) + ofy;
    
    // Clamp to 16-bit signed
    if (sx < -32768) sx = -32768; 
    if (sx > 32767) sx = 32767;
    if (sy < -32768) sy = -32768; 
    if (sy > 32767) sy = 32767;
    
    u32 sxy_val = (u16)sx | ((u16)sy << 16);

    // Store Screen Coordinates FIFO
    // SXY0 <- SXY1, SXY1 <- SXY2, SXY2 <- new
    // Actually for RTPT:
    // Call 1 (V0): Result to SXY0
    // Call 2 (V1): Result to SXY1
    // Call 3 (V2): Result to SXY2
    // But RTPS puts result into SXY2 and shifts others?
    // Let's implement specific target storage.
    
    if (v_idx == 0) cpu->cp2_data[GTE_SXY0] = sxy_val;
    else if (v_idx == 1) cpu->cp2_data[GTE_SXY1] = sxy_val;
    else cpu->cp2_data[GTE_SXY2] = sxy_val;
    
    // Also Store Z
    if (v_idx == 0) cpu->cp2_data[GTE_SZ0] = (u16)(z >> 2); // Approximation? Otz
    else if (v_idx == 1) cpu->cp2_data[GTE_SZ1] = (u16)(z >> 2);
    else cpu->cp2_data[GTE_SZ2] = (u16)(z >> 2);
    
    // Average Z (OTZ)
    // Only updated by RTPS/RTPT?
    cpu->cp2_data[GTE_OTZ] = (u16)(z >> 2); 
}

static void GTE_CMD_RTPS(R3000CPU *cpu) {
    GTE_ProcessVertex(cpu, 0);
    // RTPS puts result in SXY2
    cpu->cp2_data[GTE_SXY2] = cpu->cp2_data[GTE_SXY0];
}

static void GTE_CMD_RTPT(R3000CPU *cpu) {
    GTE_ProcessVertex(cpu, 0);
    GTE_ProcessVertex(cpu, 1);
    GTE_ProcessVertex(cpu, 2);
}

static void GTE_CMD_NCLIP(R3000CPU *cpu) {
    // Normal Clip
    // Calculates cross product of SXY0, SXY1, SXY2 to determine facing/area.
    // OP = X0*Y1 + X1*Y2 + X2*Y0 - X0*Y2 - X1*Y0 - X2*Y1
    
    s16 x0 = S16(cpu->cp2_data[GTE_SXY0]);
    s16 y0 = S16(cpu->cp2_data[GTE_SXY0] >> 16);
    s16 x1 = S16(cpu->cp2_data[GTE_SXY1]);
    s16 y1 = S16(cpu->cp2_data[GTE_SXY1] >> 16);
    s16 x2 = S16(cpu->cp2_data[GTE_SXY2]);
    s16 y2 = S16(cpu->cp2_data[GTE_SXY2] >> 16);
    
    s32 op = (x0 * y1) + (x1 * y2) + (x2 * y0) - (x0 * y2) - (x1 * y0) - (x2 * y1);
    
    cpu->cp2_data[GTE_MAC0] = op;
}

void GTE_Execute(u32 opcode, R3000CPU *cpu) {
    static int log_count = 0;
    u32 func = opcode & 0x3F;
    
    if (log_count < 200) {
        printf("[GTE] Exec: Op=%08X Func=%02X\n", opcode, func);
        log_count++;
    }

    switch (func) {
        case 0x01: // RTPS
            GTE_CMD_RTPS(cpu);
            break;
        case 0x06: // NCLIP
            GTE_CMD_NCLIP(cpu);
            break;
        case 0x30: // RTPT (Triple)
            GTE_CMD_RTPT(cpu);
            break;
        default:
            if (log_count < 200) printf("[GTE] Unknown Func %02X\n", func);
            break;
    }
}
