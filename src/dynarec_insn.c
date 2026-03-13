/*
 * dynarec_insn.c - PSX instruction emitter (main switch)
 *
 * Contains emit_instruction() which generates native R5900 code for each
 * PSX R3000A instruction, plus BIOS HLE functions and debug helpers.
 */
#include "dynarec.h"
#undef LOG_TAG
#include "gpu_state.h"
#undef LOG_TAG
#define LOG_TAG "DYNAREC"
#include "loader.h"

extern void emit_flush_partial_cycles(void);
/* ---- Debug helpers ---- */
static int mtc0_sr_log_count = 0;
static uint32_t last_sr_logged = 0xDEAD;

void debug_mtc0_sr(uint32_t val)
{
    uint32_t interesting = val & 0x00000701;
    if (interesting || mtc0_sr_log_count < 10 || val != last_sr_logged)
    {
        if (mtc0_sr_log_count < 200)
        {
            mtc0_sr_log_count++;
        }
        last_sr_logged = val;
    }
    cpu.cop0[PSX_COP0_SR] = val;
}

/*=== BIOS HLE (High Level Emulation) ===*/
int BIOS_HLE_A(void)
{
    uint32_t func = cpu.regs[9];
    static int a_log_count = 0;
    if (a_log_count < 30)
    {
        a_log_count++;
    }

    if (func == 0x3C)
    {
        char c = (char)(cpu.regs[4] & 0xFF);
        printf("%c", c);
#ifdef ENABLE_HOST_LOG
        if (host_log_fd >= 0)
        {
            host_log_putc(c);
            host_log_flush();
        }
#endif
        cpu.regs[2] = cpu.regs[4];
        cpu.pc = cpu.regs[31];
        return 1;
    }
    return 0;
}

int BIOS_HLE_B(void)
{
    uint32_t func = cpu.regs[9];
    static int b_log_count = 0;
    if (b_log_count < 30)
    {
        b_log_count++;
    }

    if (func == 0x3B)
    {
        char c = (char)(cpu.regs[4] & 0xFF);
        printf("%c", c);
#ifdef ENABLE_HOST_LOG
        if (host_log_fd >= 0)
        {
            host_log_putc(c);
            host_log_flush();
        }
#endif
        cpu.regs[2] = cpu.regs[4];
        cpu.pc = cpu.regs[31];
        return 1;
    }
    if (func == 0x3D)
    {
        char c = (char)(cpu.regs[4] & 0xFF);
        printf("%c", c);
#ifdef ENABLE_HOST_LOG
        if (host_log_fd >= 0)
        {
            host_log_putc(c);
            host_log_flush();
        }
#endif
        cpu.regs[2] = 1;
        cpu.pc = cpu.regs[31];
        return 1;
    }
    return 0;
}

int BIOS_HLE_C(void)
{
    uint32_t func = cpu.regs[9];
    static int c_log_count = 0;
    if (c_log_count < 20)
    {
        c_log_count++;
    }
    (void)func;
    return 0;
}

/* ================================================================
 * GTE inline helper emitters
 * ================================================================
 * These emit common GTE code sequences shared by multiple ops.
 * Register conventions:
 *   V0=MAC1, V1=MAC2, A0=MAC3 (input/output for push_color + IR sat)
 *   S0=cpu pointer (always valid)
 */

/* Emit push_color: RGB FIFO shift + color pack from MAC>>4.
 * Expects: V0=MAC1, V1=MAC2, A0=MAC3.
 * Preserves: V0, V1, A0.
 * Clobbers: A1, A2, A3, T8, T9, AT. (29 words) */
static void emit_push_color_inline(void)
{
    /* RGB FIFO shift: RGB0=RGB1, RGB1=RGB2 */
    EMIT_LW(REG_T8, CPU_CP2_DATA(21), REG_S0);
    EMIT_LW(REG_T9, CPU_CP2_DATA(22), REG_S0);
    EMIT_SW(REG_T8, CPU_CP2_DATA(20), REG_S0);
    EMIT_SW(REG_T9, CPU_CP2_DATA(21), REG_S0);

    /* Color: r/g/b = clamp(MAC>>4, 0, 255) — P19: PMAXW/PMINW */
    EMIT_SRA(REG_A1, REG_V0, 4);
    EMIT_SRA(REG_A2, REG_V1, 4);
    EMIT_SRA(REG_A3, REG_A0, 4);

    EMIT_ORI(REG_T9, REG_ZERO, 0xFF);
    EMIT_PMAXW(REG_A1, REG_A1, REG_ZERO);
    EMIT_PMINW(REG_A1, REG_A1, REG_T9);
    EMIT_PMAXW(REG_A2, REG_A2, REG_ZERO);
    EMIT_PMINW(REG_A2, REG_A2, REG_T9);
    EMIT_PMAXW(REG_A3, REG_A3, REG_ZERO);
    EMIT_PMINW(REG_A3, REG_A3, REG_T9);

    /* Code byte from RGBC */
    EMIT_LW(REG_AT, CPU_CP2_DATA(6), REG_S0);
    emit(MK_R(0, 0, REG_AT, REG_AT, 24, 0x02));       /* SRL AT, AT, 24 */

    /* Pack: RGB2 = r | (g<<8) | (b<<16) | (code<<24) */
    EMIT_SLL(REG_AT, REG_AT, 24);
    EMIT_SLL(REG_A3, REG_A3, 16);
    EMIT_OR(REG_AT, REG_AT, REG_A3);
    EMIT_SLL(REG_A2, REG_A2, 8);
    EMIT_OR(REG_AT, REG_AT, REG_A2);
    EMIT_OR(REG_AT, REG_AT, REG_A1);
    EMIT_SW(REG_AT, CPU_CP2_DATA(22), REG_S0);
}

/* Emit IR saturation + store IR1-3 + FLAG=0.
 * Expects: V0=MAC1, V1=MAC2, A0=MAC3.
 * lm=0: clamp [-0x8000, 0x7FFF] (8+4=12w)
 * lm=1: clamp [0, 0x7FFF] (7+4=11w) */
static void emit_ir_sat_store(int lm)
{
    /* P19: Use R5900 MMI PMAXW/PMINW for per-word signed clamp.
     * Only lane 0 (lower 32 bits) matters; upper lanes are don't-care.
     * PMAXW/PMINW execute in 1 cycle each, replacing 4-word SLT+MOVN
     * per channel (saves 6 emitted words). */
    EMIT_ORI(REG_T9, REG_ZERO, 0x7FFF);
    if (lm) {
        /* lm=1: clamp [0, 0x7FFF] */
        EMIT_PMAXW(REG_V0, REG_V0, REG_ZERO);
        EMIT_PMINW(REG_V0, REG_V0, REG_T9);
        EMIT_PMAXW(REG_V1, REG_V1, REG_ZERO);
        EMIT_PMINW(REG_V1, REG_V1, REG_T9);
        EMIT_PMAXW(REG_A0, REG_A0, REG_ZERO);
        EMIT_PMINW(REG_A0, REG_A0, REG_T9);
    } else {
        /* lm=0: clamp [-0x8000, 0x7FFF] */
        EMIT_ADDIU(REG_T8, REG_ZERO, -0x8000);
        EMIT_PMAXW(REG_V0, REG_V0, REG_T8);
        EMIT_PMINW(REG_V0, REG_V0, REG_T9);
        EMIT_PMAXW(REG_V1, REG_V1, REG_T8);
        EMIT_PMINW(REG_V1, REG_V1, REG_T9);
        EMIT_PMAXW(REG_A0, REG_A0, REG_T8);
        EMIT_PMINW(REG_A0, REG_A0, REG_T9);
    }

    EMIT_SW(REG_V0, CPU_CP2_DATA(9),  REG_S0);
    EMIT_SW(REG_V1, CPU_CP2_DATA(10), REG_S0);
    EMIT_SW(REG_A0, CPU_CP2_DATA(11), REG_S0);
    EMIT_SW(REG_ZERO, CPU_CP2_CTRL(31), REG_S0);
}

/* Emit interpolate_color_acc: result = (FC - acc) * IR0 + acc, then MAC >> sf*12.
 * Expects: V0=acc1, V1=acc2, A0=acc3.
 * Output: V0=MAC1, V1=MAC2, A0=MAC3 (stored to memory too).
 * Clobbers: A1, A2, A3, T8, T9, AT.
 * sf=0: ~47 words, sf=1: ~53 words */
static void emit_interpolate_color(int sf)
{
    /* Load far color (FC) and shift << 12 */
    EMIT_LW(REG_A1, CPU_CP2_CTRL(21), REG_S0);        /* A1 = RFC */
    EMIT_LW(REG_A2, CPU_CP2_CTRL(22), REG_S0);        /* A2 = GFC */
    EMIT_LW(REG_A3, CPU_CP2_CTRL(23), REG_S0);        /* A3 = BFC */
    EMIT_SLL(REG_A1, REG_A1, 12);                      /* A1 = fc1 = RFC<<12 */
    EMIT_SLL(REG_A2, REG_A2, 12);                      /* A2 = fc2 = GFC<<12 */
    EMIT_SLL(REG_A3, REG_A3, 12);                      /* A3 = fc3 = BFC<<12 */

    /* diff = fc - acc (V0/V1/A0 still hold acc) */
    EMIT_SUBU(REG_A1, REG_A1, REG_V0);                /* A1 = diff1 */
    EMIT_SUBU(REG_A2, REG_A2, REG_V1);                /* A2 = diff2 */
    EMIT_SUBU(REG_A3, REG_A3, REG_A0);                /* A3 = diff3 */

    /* sf=1: shift diff >> 12 */
    if (sf) {
        EMIT_SRA(REG_A1, REG_A1, 12);
        EMIT_SRA(REG_A2, REG_A2, 12);
        EMIT_SRA(REG_A3, REG_A3, 12);
    }

    /* Saturate intermediate to [-0x8000, 0x7FFF] — P19: PMAXW/PMINW */
    EMIT_ADDIU(REG_T8, REG_ZERO, -0x8000);
    EMIT_ORI(REG_T9, REG_ZERO, 0x7FFF);
    EMIT_PMAXW(REG_A1, REG_A1, REG_T8);
    EMIT_PMINW(REG_A1, REG_A1, REG_T9);
    EMIT_PMAXW(REG_A2, REG_A2, REG_T8);
    EMIT_PMINW(REG_A2, REG_A2, REG_T9);
    EMIT_PMAXW(REG_A3, REG_A3, REG_T8);
    EMIT_PMINW(REG_A3, REG_A3, REG_T9);

    /* result = tmp_ir × IR0 + acc */
    EMIT_LH(REG_T8, CPU_CP2_DATA(8), REG_S0);         /* T8 = IR0 (int16) */

    EMIT_MULT(REG_A1, REG_T8);
    EMIT_MFLO(REG_A1);
    EMIT_ADDU(REG_V0, REG_A1, REG_V0);                /* V0 = product1 + acc1 */

    EMIT_MULT(REG_A2, REG_T8);
    EMIT_MFLO(REG_A2);
    EMIT_ADDU(REG_V1, REG_A2, REG_V1);                /* V1 = product2 + acc2 */

    EMIT_MULT(REG_A3, REG_T8);
    EMIT_MFLO(REG_A3);
    EMIT_ADDU(REG_A0, REG_A3, REG_A0);                /* A0 = product3 + acc3 */

    /* MAC = result >> sf*12 */
    if (sf) {
        EMIT_SRA(REG_V0, REG_V0, 12);
        EMIT_SRA(REG_V1, REG_V1, 12);
        EMIT_SRA(REG_A0, REG_A0, 12);
    }

    /* Store MAC1-3 */
    EMIT_SW(REG_V0, CPU_CP2_DATA(25), REG_S0);
    EMIT_SW(REG_V1, CPU_CP2_DATA(26), REG_S0);
    EMIT_SW(REG_A0, CPU_CP2_DATA(27), REG_S0);
}

/* ---- P18: Shared VU0 matrix loading for ×3 commands ----
 * When vu0_preloaded[mx] != 0, a ×3 caller has already loaded the matrix
 * into VF registers starting at that base.  emit_inline_mvmva skips the
 * C call + LQC2 and uses the preloaded VF registers directly.
 * Index: 0=RT, 1=Light, 2=Color. */
#if !defined(ENABLE_VU0_MICRO) && defined(PLATFORM_PS2)
static int vu0_preloaded[3] = {0, 0, 0};

/* Emit C call to refresh matrix cache + load into VF[vf_base..vf_base+3].
 * After call: vu0_jit_cache contains the matrix, VF regs loaded, T8 clobbered.
 * Emits 12 words. */
static void emit_vu0_load_matrix(int mx, int cv, int vf_base)
{
    uint32_t mx_cv = (uint32_t)(mx | (cv << 2));
    EMIT_MOVE(REG_A0, REG_S0);
    EMIT_ORI(REG_A1, REG_ZERO, mx_cv);
    emit_call_c_lite((uint32_t)(uintptr_t)vu0_prepare_mvmva);
    emit_load_imm32(REG_T8, (uint32_t)(uintptr_t)&vu0_jit_cache);
    EMIT_LQC2(vf_base,     0,  REG_T8);   /* col1   */
    EMIT_LQC2(vf_base + 1, 16, REG_T8);   /* col2   */
    EMIT_LQC2(vf_base + 2, 32, REG_T8);   /* col3   */
    EMIT_LQC2(vf_base + 3, 48, REG_T8);   /* trans  */
}

/* Emit VU0 vertex multiply using pre-loaded matrix in VF[vf_col1..vf_col1+3].
 * If t8_valid=0, loads T8 = &vu0_jit_cache (2 extra words).
 * VF5 = vertex, VF6 = result.
 * Output: V0=MAC1, V1=MAC2, A0=MAC3 stored + IR1-3 saturated + FLAG=0.
 * Clobbers: T8, T9, AT, V0, V1, A0, $f0-$f2. */
static void emit_vu0_vertex_multiply(int v, int lm, int vf_col1, int t8_valid)
{
    if (!t8_valid)
        emit_load_imm32(REG_T8, (uint32_t)(uintptr_t)&vu0_jit_cache);

    /* Vertex int16 → FPU float → scratch → LQC2 VF5. */
    if (v < 3) {
        int base = v * 2;
        EMIT_LH(REG_V0, CPU_CP2_DATA(base) + 0, REG_S0);
        EMIT_LH(REG_V1, CPU_CP2_DATA(base) + 2, REG_S0);
        EMIT_LH(REG_A0, CPU_CP2_DATA(base + 1), REG_S0);
    } else {
        EMIT_LH(REG_V0, CPU_CP2_DATA(9),  REG_S0);
        EMIT_LH(REG_V1, CPU_CP2_DATA(10), REG_S0);
        EMIT_LH(REG_A0, CPU_CP2_DATA(11), REG_S0);
    }
    EMIT_MTC1(REG_V0, 0);          EMIT_CVT_S_W(0, 0);
    EMIT_MTC1(REG_V1, 1);          EMIT_CVT_S_W(1, 1);
    EMIT_MTC1(REG_A0, 2);          EMIT_CVT_S_W(2, 2);
    EMIT_SWC1(0, 64, REG_T8);
    EMIT_SWC1(1, 68, REG_T8);
    EMIT_SWC1(2, 72, REG_T8);
    EMIT_LQC2(5, 64, REG_T8);

    /* VU0 matrix × vector + translation → VF6. */
    EMIT_VMULAX_XYZ(vf_col1, 5);
    EMIT_VMADDAY_XYZ(vf_col1 + 1, 5);
    EMIT_VMADDZ_XYZ(6, vf_col1 + 2, 5);
    EMIT_VADD_XYZ(6, 6, vf_col1 + 3);

    /* Store result → scratch → FPU extract → GPR. */
    EMIT_SQC2(6, 64, REG_T8);
    EMIT_LWC1(0, 64, REG_T8);   EMIT_CVT_W_S(0, 0);  EMIT_MFC1(REG_V0, 0);
    EMIT_LWC1(1, 68, REG_T8);   EMIT_CVT_W_S(1, 1);  EMIT_MFC1(REG_V1, 1);
    EMIT_LWC1(2, 72, REG_T8);   EMIT_CVT_W_S(2, 2);  EMIT_MFC1(REG_A0, 2);

    /* Store MAC1-3 + IR saturation + FLAG=0. */
    EMIT_SW(REG_V0, CPU_CP2_DATA(25), REG_S0);
    EMIT_SW(REG_V1, CPU_CP2_DATA(26), REG_S0);
    EMIT_SW(REG_A0, CPU_CP2_DATA(27), REG_S0);
    emit_ir_sat_store(lm);
}
#endif /* !ENABLE_VU0_MICRO && PLATFORM_PS2 */

#ifdef ENABLE_VU0_MICRO
#include "vu0_micro_ps2.h"

/* ---- VU0 Micro Mode: matrix × vector via micro program ----
 *
 * Instead of the macro pipeline (int→FPU→VU0 macro→FPU→int = ~35 words),
 * we write vertex int32 to VU0 data memory, launch a micro program via
 * VCALLMS, poll for completion, and read the int32 result back.
 * The micro program handles ITOF0, matrix multiply, FTOI0 internally.
 *
 * For ×3 variants, the first vertex uses _FULL (loads matrix from data mem
 * into VF1-4), subsequent vertices use _CORE (reuses VF1-4).
 *
 * vu0_micro_preloaded[mx]:
 *   0 = matrix not yet in VU0 data mem → needs prepare_matrix + _FULL
 *   1 = matrix in VU0 data mem + VF regs → can use _CORE
 */
static int vu0_micro_preloaded[3] = {0, 0, 0};

/* Emit: refresh matrix in VU0 data memory via C call.
 * mx: 0=RT, 1=Light, 2=Color.  cv: 0=TR, 1=BK, 3=none.
 * After call, the 4 QWs (col1,col2,col3,trans) are in VU data mem. */
static void emit_vu0_micro_prepare(int mx, int cv)
{
    uint32_t mx_cv = (uint32_t)(mx | (cv << 2));
    EMIT_MOVE(REG_A0, REG_S0);
    EMIT_ORI(REG_A1, REG_ZERO, mx_cv);
    emit_call_c_lite((uint32_t)(uintptr_t)vu0_micro_prepare_matrix);
}

/* Emit: write vertex to VU0 data memory QW[0], launch micro, poll, read result.
 * v: 0-2=V0/V1/V2, 3=IR1-3.  lm: limit flag.  use_core: 1=reuse VF1-4.
 * After: V0=MAC1, V1=MAC2, A0=MAC3 stored to cp2_data[25-27], IR1-3 sat, FLAG=0.
 * Clobbers: T8, T9, AT, V0, V1, A0. */
static void emit_vu0_micro_multiply(int v, int lm, int use_core)
{
    /* Load vertex from cpu struct */
    if (v < 3) {
        int base = v * 2;
        EMIT_LH(REG_V0, CPU_CP2_DATA(base) + 0, REG_S0);     /* VX */
        EMIT_LH(REG_V1, CPU_CP2_DATA(base) + 2, REG_S0);     /* VY */
        EMIT_LH(REG_A0, CPU_CP2_DATA(base + 1), REG_S0);     /* VZ */
    } else {
        /* v=3: use IR1-3 as vector (for Color matrix step) */
        EMIT_LH(REG_V0, CPU_CP2_DATA(9),  REG_S0);           /* IR1 */
        EMIT_LH(REG_V1, CPU_CP2_DATA(10), REG_S0);           /* IR2 */
        EMIT_LH(REG_A0, CPU_CP2_DATA(11), REG_S0);           /* IR3 */
    }

    /* Write vertex to VU0 data memory QW[0] as int32.
     * VU0 data mem at 0x11004000 (uncached, no sync needed for writes). */
    emit_load_imm32(REG_T8, VU0_DATA_MEM);
    EMIT_SW(REG_V0, VU0_OFF_VERTEX + 0,  REG_T8);            /* x */
    EMIT_SW(REG_V1, VU0_OFF_VERTEX + 4,  REG_T8);            /* y */
    EMIT_SW(REG_A0, VU0_OFF_VERTEX + 8,  REG_T8);            /* z */
    EMIT_SW(REG_ZERO, VU0_OFF_VERTEX + 12, REG_T8);          /* w=0 */

    /* Launch micro program: CTC2 sets CMSAR0, VCALLMSR starts execution */
    int prog_addr = use_core ? VU0_PROG_MVMVA_CORE : VU0_PROG_MVMVA_FULL;
    EMIT_ORI(REG_T9, REG_ZERO, prog_addr >> 3);
    EMIT_CTC2(REG_T9, 27);                                    /* CMSAR0 = addr */
    EMIT_VCALLMSR();                                           /* launch VU0 */

    /* Poll VU0 completion: CFC2 $t9, $vi29; ANDI $t9, 1; BNE → loop.
     * VU0 MVMVA is ~17 insns @ 1 cycle each ≈ 17 VU cycles.
     * EE may need 1-3 poll iterations. */
    EMIT_CFC2(REG_T9, 29);                                    /* T9 = VU0 status */
    EMIT_ANDI(REG_T9, REG_T9, 1);                             /* bit 0 = VBS0 */
    EMIT_BNE(REG_T9, REG_ZERO, -3);                           /* branch back to CFC2 */
    EMIT_NOP();                                                /* delay slot */

    /* Read result from VU data mem QW[15] = MAC1, MAC2, MAC3, 0 */
    /* T8 still = VU0_DATA_MEM from above */
    EMIT_LW(REG_V0, VU0_OFF_OUT_MAC + 0,  REG_T8);           /* MAC1 */
    EMIT_LW(REG_V1, VU0_OFF_OUT_MAC + 4,  REG_T8);           /* MAC2 */
    EMIT_LW(REG_A0, VU0_OFF_OUT_MAC + 8,  REG_T8);           /* MAC3 */

    /* Store MAC1-3 + IR saturation + FLAG=0 */
    EMIT_SW(REG_V0, CPU_CP2_DATA(25), REG_S0);
    EMIT_SW(REG_V1, CPU_CP2_DATA(26), REG_S0);
    EMIT_SW(REG_A0, CPU_CP2_DATA(27), REG_S0);
    emit_ir_sat_store(lm);
}

/* Emit: write vertex to VU0 data mem + launch micro (no poll/read).
 * Used for overlapped x3 variants: launch next vertex while EE processes previous.
 * Clobbers: T8, T9, V0, V1, A0. */
static void emit_vu0_micro_launch(int v, int use_core)
{
    if (v < 3) {
        int base = v * 2;
        EMIT_LH(REG_V0, CPU_CP2_DATA(base) + 0, REG_S0);
        EMIT_LH(REG_V1, CPU_CP2_DATA(base) + 2, REG_S0);
        EMIT_LH(REG_A0, CPU_CP2_DATA(base + 1), REG_S0);
    } else {
        EMIT_LH(REG_V0, CPU_CP2_DATA(9),  REG_S0);
        EMIT_LH(REG_V1, CPU_CP2_DATA(10), REG_S0);
        EMIT_LH(REG_A0, CPU_CP2_DATA(11), REG_S0);
    }
    emit_load_imm32(REG_T8, VU0_DATA_MEM);
    EMIT_SW(REG_V0, VU0_OFF_VERTEX + 0,  REG_T8);
    EMIT_SW(REG_V1, VU0_OFF_VERTEX + 4,  REG_T8);
    EMIT_SW(REG_A0, VU0_OFF_VERTEX + 8,  REG_T8);
    EMIT_SW(REG_ZERO, VU0_OFF_VERTEX + 12, REG_T8);
    int prog_addr = use_core ? VU0_PROG_MVMVA_CORE : VU0_PROG_MVMVA_FULL;
    EMIT_ORI(REG_T9, REG_ZERO, prog_addr >> 3);
    EMIT_CTC2(REG_T9, 27);
    EMIT_VCALLMSR();
}

/* Emit: poll VU0 completion + read MAC1-3 from VU data mem + store + IR sat.
 * Clobbers: T8, T9, V0, V1, A0. */
static void emit_vu0_micro_poll_complete(int lm)
{
    EMIT_CFC2(REG_T9, 29);
    EMIT_ANDI(REG_T9, REG_T9, 1);
    EMIT_BNE(REG_T9, REG_ZERO, -3);
    EMIT_NOP();
    emit_load_imm32(REG_T8, VU0_DATA_MEM);
    EMIT_LW(REG_V0, VU0_OFF_OUT_MAC + 0,  REG_T8);
    EMIT_LW(REG_V1, VU0_OFF_OUT_MAC + 4,  REG_T8);
    EMIT_LW(REG_A0, VU0_OFF_OUT_MAC + 8,  REG_T8);
    EMIT_SW(REG_V0, CPU_CP2_DATA(25), REG_S0);
    EMIT_SW(REG_V1, CPU_CP2_DATA(26), REG_S0);
    EMIT_SW(REG_A0, CPU_CP2_DATA(27), REG_S0);
    emit_ir_sat_store(lm);
}
#endif /* ENABLE_VU0_MICRO */

/* Emit inline MVMVA: MAC = Matrix × Vector + Translation, then store MAC+IR.
 * mx: 0=RT, 1=Light(L), 2=Color(LC)
 * v:  0=V0, 1=V1, 2=V2, 3=IR1-3
 * cv: 0=TR, 1=BK, 3=nothing (cv=2/FC bugged path not inlined)
 * sf/lm: compile-time flags
 *
 * Output: V0=MAC1, V1=MAC2, A0=MAC3 (stored to memory). IR1-3 stored.
 * Uses MULT/MADD for 3-row dot products (7 words per row).
 * Clobbers: T8, T9, AT, V0, V1, A0, A1, A2, A3.
 *
 * Matrix ctrl layout (packed int16 pairs):
 *   ctrl[b+0] = M11|M12, ctrl[b+1] = M13|M21
 *   ctrl[b+2] = M22|M23, ctrl[b+3] = M31|M32
 *   ctrl[b+4] = M33 (lo16 only)
 * where b=0(RT), 8(Light), 16(Color)
 */
static void emit_inline_mvmva(int mx, int v, int cv, int sf, int lm)
{
    /* VU0 fast path: FPU int↔float conversion + VU0 matrix multiply.
     * Only for sf=1 (matrix pre-scaled by 1/4096 in the cache).
     * Results: V0=MAC1, V1=MAC2, A0=MAC3 → cp2_data[25-27], IR1-3, FLAG=0.
     * Uses: $f0-$f2 (FPU scratch), T8 (base addr). */
    if (sf) {
#ifdef ENABLE_VU0_MICRO
        /* VU0 micro path: write vertex to VU data mem, launch micro program.
         * vu0_micro_preloaded[mx] tracks if matrix is in VU data mem + VF regs. */
        int micro_ready = (mx < 3) ? vu0_micro_preloaded[mx] : 0;
        if (micro_ready) {
            /* Matrix already in VU data mem + VF1-4 from prior _FULL call → use _CORE */
            emit_vu0_micro_multiply(v, lm, 1);
        } else {
            /* Prepare matrix in VU0 data mem, then launch _FULL (loads into VF1-4) */
            emit_vu0_micro_prepare(mx, cv);
            emit_vu0_micro_multiply(v, lm, 0);
        }
#elif defined(PLATFORM_PS2)
        int vf_base = (mx < 3) ? vu0_preloaded[mx] : 0;
        if (vf_base) {
            /* P18: matrix already in VF[vf_base..vf_base+3], skip prepare.
             * T8 was clobbered since matrix load → reload needed. */
            emit_vu0_vertex_multiply(v, lm, vf_base, 0);
        } else {
            /* Standalone call: load matrix + compute.
             * T8 still valid from emit_vu0_load_matrix → skip reload. */
            emit_vu0_load_matrix(mx, cv, 1);
            emit_vu0_vertex_multiply(v, lm, 1, 1);
        }
#endif /* ENABLE_VU0_MICRO / PLATFORM_PS2 */
        return;
    }
    /* --- Integer fallback (sf=0 — rare, e.g. standalone MVMVA) --- */
    /* Load vector into V0/V1/A0 */
    if (v < 3) {
        int base = v * 2;  /* data[0,2,4] for V0,V1,V2 */
        EMIT_LH(REG_V0, CPU_CP2_DATA(base) + 0, REG_S0);     /* VX */
        EMIT_LH(REG_V1, CPU_CP2_DATA(base) + 2, REG_S0);     /* VY */
        EMIT_LH(REG_A0, CPU_CP2_DATA(base + 1), REG_S0);     /* VZ */
    } else {
        /* v=3: IR1-3 as vector */
        EMIT_LH(REG_V0, CPU_CP2_DATA(9),  REG_S0);
        EMIT_LH(REG_V1, CPU_CP2_DATA(10), REG_S0);
        EMIT_LH(REG_A0, CPU_CP2_DATA(11), REG_S0);
    }

    /* Matrix base in ctrl regs */
    int cb = (mx == 0) ? 0 : (mx == 1) ? 8 : 16;

    /* Row 1: M11*VX + M12*VY + M13*VZ */
    EMIT_LH(REG_T8, CPU_CP2_CTRL(cb) + 0, REG_S0);       /* M11 */
    EMIT_MULT(REG_T8, REG_V0);                             /* HI:LO = M11*VX */
    EMIT_LH(REG_T8, CPU_CP2_CTRL(cb) + 2, REG_S0);       /* M12 */
    EMIT_MADD(REG_T8, REG_V1);                             /* HI:LO += M12*VY */
    EMIT_LH(REG_T8, CPU_CP2_CTRL(cb+1) + 0, REG_S0);     /* M13 */
    EMIT_MADD(REG_T8, REG_A0);                             /* HI:LO += M13*VZ */
    EMIT_MFLO(REG_A1);                                     /* A1 = row1 */

    /* Row 2: M21*VX + M22*VY + M23*VZ */
    EMIT_LH(REG_T8, CPU_CP2_CTRL(cb+1) + 2, REG_S0);     /* M21 */
    EMIT_MULT(REG_T8, REG_V0);
    EMIT_LH(REG_T8, CPU_CP2_CTRL(cb+2) + 0, REG_S0);     /* M22 */
    EMIT_MADD(REG_T8, REG_V1);
    EMIT_LH(REG_T8, CPU_CP2_CTRL(cb+2) + 2, REG_S0);     /* M23 */
    EMIT_MADD(REG_T8, REG_A0);
    EMIT_MFLO(REG_A2);                                     /* A2 = row2 */

    /* Row 3: M31*VX + M32*VY + M33*VZ */
    EMIT_LH(REG_T8, CPU_CP2_CTRL(cb+3) + 0, REG_S0);     /* M31 */
    EMIT_MULT(REG_T8, REG_V0);
    EMIT_LH(REG_T8, CPU_CP2_CTRL(cb+3) + 2, REG_S0);     /* M32 */
    EMIT_MADD(REG_T8, REG_V1);
    EMIT_LH(REG_T8, CPU_CP2_CTRL(cb+4) + 0, REG_S0);     /* M33 */
    EMIT_MADD(REG_T8, REG_A0);
    EMIT_MFLO(REG_A3);                                     /* A3 = row3 */

    /* Add translation vector (shifted << 12 before adding to raw products) */
    if (cv == 0) {
        /* TR: ctrl[5-7] */
        EMIT_LW(REG_T8, CPU_CP2_CTRL(5), REG_S0);
        EMIT_SLL(REG_T8, REG_T8, 12);
        EMIT_ADDU(REG_A1, REG_A1, REG_T8);
        EMIT_LW(REG_T8, CPU_CP2_CTRL(6), REG_S0);
        EMIT_SLL(REG_T8, REG_T8, 12);
        EMIT_ADDU(REG_A2, REG_A2, REG_T8);
        EMIT_LW(REG_T8, CPU_CP2_CTRL(7), REG_S0);
        EMIT_SLL(REG_T8, REG_T8, 12);
        EMIT_ADDU(REG_A3, REG_A3, REG_T8);
    } else if (cv == 1) {
        /* BK: ctrl[13-15] */
        EMIT_LW(REG_T8, CPU_CP2_CTRL(13), REG_S0);
        EMIT_SLL(REG_T8, REG_T8, 12);
        EMIT_ADDU(REG_A1, REG_A1, REG_T8);
        EMIT_LW(REG_T8, CPU_CP2_CTRL(14), REG_S0);
        EMIT_SLL(REG_T8, REG_T8, 12);
        EMIT_ADDU(REG_A2, REG_A2, REG_T8);
        EMIT_LW(REG_T8, CPU_CP2_CTRL(15), REG_S0);
        EMIT_SLL(REG_T8, REG_T8, 12);
        EMIT_ADDU(REG_A3, REG_A3, REG_T8);
    }
    /* cv=3: no translation */

    /* sf shift */
    if (sf) {
        EMIT_SRA(REG_A1, REG_A1, 12);
        EMIT_SRA(REG_A2, REG_A2, 12);
        EMIT_SRA(REG_A3, REG_A3, 12);
    }

    /* Move to V0/V1/A0 (needed for push_color/IR_sat helpers) */
    EMIT_MOVE(REG_V0, REG_A1);
    EMIT_MOVE(REG_V1, REG_A2);
    EMIT_MOVE(REG_A0, REG_A3);

    /* Store MAC1-3 */
    EMIT_SW(REG_V0, CPU_CP2_DATA(25), REG_S0);
    EMIT_SW(REG_V1, CPU_CP2_DATA(26), REG_S0);
    EMIT_SW(REG_A0, CPU_CP2_DATA(27), REG_S0);

    /* IR saturation + store IR1-3 */
    /* Need IR for step 2 vectorv=3 reads, so must store IR here */
    emit_ir_sat_store(lm);
}

/* Emit RGBC×IR<<4 accumulator computation.
 * Reads RGBC + IR1-3 from memory, computes acc = (rgb_byte × ir_value) << 4.
 * Output: V0=acc1, V1=acc2, A0=acc3.
 * Clobbers: T8, T9, A1, A2, A3. (~18 words) */
static void emit_rgbc_times_ir_shl4(void)
{
    /* Extract R,G,B from RGBC */
    EMIT_LW(REG_T8, CPU_CP2_DATA(6), REG_S0);
    EMIT_ANDI(REG_V0, REG_T8, 0xFF);                      /* V0 = R */
    emit(MK_R(0, 0, REG_T8, REG_T9, 8, 0x02));           /* SRL T9,T8,8 */
    EMIT_ANDI(REG_V1, REG_T9, 0xFF);                      /* V1 = G */
    emit(MK_R(0, 0, REG_T8, REG_T9, 16, 0x02));          /* SRL T9,T8,16 */
    EMIT_ANDI(REG_A0, REG_T9, 0xFF);                      /* A0 = B */

    /* Load IR1-3 */
    EMIT_LH(REG_A1, CPU_CP2_DATA(9),  REG_S0);           /* A1 = IR1 */
    EMIT_LH(REG_A2, CPU_CP2_DATA(10), REG_S0);           /* A2 = IR2 */
    EMIT_LH(REG_A3, CPU_CP2_DATA(11), REG_S0);           /* A3 = IR3 */

    /* acc = (rgb × ir) << 4 */
    EMIT_MULT(REG_V0, REG_A1); EMIT_MFLO(REG_V0);
    EMIT_SLL(REG_V0, REG_V0, 4);                          /* V0 = (R*IR1)<<4 */

    EMIT_MULT(REG_V1, REG_A2); EMIT_MFLO(REG_V1);
    EMIT_SLL(REG_V1, REG_V1, 4);                          /* V1 = (G*IR2)<<4 */

    EMIT_MULT(REG_A0, REG_A3); EMIT_MFLO(REG_A0);
    EMIT_SLL(REG_A0, REG_A0, 4);                          /* A0 = (B*IR3)<<4 */
}

/* Emit store_mac_ir + push_color + FLAG=0 for ops that need
 * MAC store → push_color → IR saturation (NCCS, CC pattern).
 * Expects: V0=product1, V1=product2, A0=product3 (raw values).
 * sf: shift by 12 for MAC. lm: IR saturation bound. */
static void emit_sf_mac_push_ir(int sf, int lm)
{
    if (sf) {
        EMIT_SRA(REG_V0, REG_V0, 12);
        EMIT_SRA(REG_V1, REG_V1, 12);
        EMIT_SRA(REG_A0, REG_A0, 12);
    }
    EMIT_SW(REG_V0, CPU_CP2_DATA(25), REG_S0);
    EMIT_SW(REG_V1, CPU_CP2_DATA(26), REG_S0);
    EMIT_SW(REG_A0, CPU_CP2_DATA(27), REG_S0);
    emit_push_color_inline();
    emit_ir_sat_store(lm);
}

/* Emit NCS core for vertex v: Light×V + BK+Color×IR → push_color.
 * 2× mvmva + reload MAC + push_color. (~150 words) */
static void emit_ncs_core(int v, int sf, int lm)
{
    emit_inline_mvmva(1, v, 3, sf, lm);   /* Light × V, no TR */
    emit_inline_mvmva(2, 3, 1, sf, lm);   /* BK + Color × IR */
    /* mvmva left saturated IR in V0/V1/A0; reload MAC for push_color */
    EMIT_LW(REG_V0, CPU_CP2_DATA(25), REG_S0);
    EMIT_LW(REG_V1, CPU_CP2_DATA(26), REG_S0);
    EMIT_LW(REG_A0, CPU_CP2_DATA(27), REG_S0);
    emit_push_color_inline();
    /* FLAG=0 already set by emit_ir_sat_store inside mvmva */
}

/* Emit NCCS core for vertex v: 2× mvmva + RGBC×IR<<4 → store_mac_ir + push_color.
 * (~190 words) */
static void emit_nccs_core(int v, int sf, int lm)
{
    emit_inline_mvmva(1, v, 3, sf, lm);
    emit_inline_mvmva(2, 3, 1, sf, lm);
    emit_rgbc_times_ir_shl4();             /* V0/V1/A0 = (rgb×ir)<<4 */
    emit_sf_mac_push_ir(sf, lm);           /* sf + MAC + push_color + IR sat + FLAG=0 */
}

/* Emit NCDS core for vertex v: 2× mvmva + RGBC×IR<<4 → interpolate + push_color.
 * (~230 words) */
static void emit_ncds_core(int v, int sf, int lm)
{
    emit_inline_mvmva(1, v, 3, sf, lm);
    emit_inline_mvmva(2, 3, 1, sf, lm);
    emit_rgbc_times_ir_shl4();             /* V0/V1/A0 = acc = (rgb×ir)<<4 */
    emit_interpolate_color(sf);            /* FC interpolation, stores MAC */
    emit_push_color_inline();
    emit_ir_sat_store(lm);                 /* FLAG=0 */
}

#ifdef ENABLE_VU0_MICRO
/* Post-lighting helpers for overlapped ×3 variants.
 * These emit ONLY the tail after both mvmva calls have completed.
 * All read inputs from cp2_data[] (memory), safe after launch clobbers regs. */

/* NCS post-lighting: reload MAC + push_color. (~32 words) */
static void emit_ncs_post_lighting(void)
{
    EMIT_LW(REG_V0, CPU_CP2_DATA(25), REG_S0);
    EMIT_LW(REG_V1, CPU_CP2_DATA(26), REG_S0);
    EMIT_LW(REG_A0, CPU_CP2_DATA(27), REG_S0);
    emit_push_color_inline();
}

/* NCCS post-lighting: RGBC×IR<<4 + sf_mac_push_ir. (~40 words) */
static void emit_nccs_post_lighting(int sf, int lm)
{
    emit_rgbc_times_ir_shl4();
    emit_sf_mac_push_ir(sf, lm);
}

/* NCDS post-lighting: RGBC×IR<<4 + interpolate + push_color + IR sat. (~111 words) */
static void emit_ncds_post_lighting(int sf, int lm)
{
    emit_rgbc_times_ir_shl4();
    emit_interpolate_color(sf);
    emit_push_color_inline();
    emit_ir_sat_store(lm);
}
#endif /* ENABLE_VU0_MICRO */

/* Emit RTPS core for vertex v: fully inline — matrix multiply, SZ push,
 * UNR division, screen projection, SXY push, and depth cueing.
 * last=1 → also compute MAC0/IR0 (depth cueing).
 *
 * Register plan (post-mvmva):
 *   T8/T9/AT = scratch, V0 = H, V1 = cond(h<sz3*2),
 *   A0 = n→div_result, A1 = z/scratch, A2 = u_val, A3 = table_base
 *
 * MIPS encodings used (no EMIT_ macro):
 *   SLLV  rd,rt,rs: MK_R(0, rs, rt, rd, 0, 0x04)
 *   SLTIU rt,rs,imm: MK_I(0x0B, rs, rt, imm)
 *   SLTU  rd,rs,rt: MK_R(0, rs, rt, rd, 0, 0x2B)
 *   SLT   rd,rs,rt: MK_R(0, rs, rt, rd, 0, 0x2A)
 *   MULTU rs,rt:    MK_R(0, rs, rt, 0, 0, 0x19)
 *   XORI  rt,rs,imm: MK_I(0x0E, rs, rt, imm)
 *   SRL   rd,rt,sa: MK_R(0, 0, rt, rd, sa, 0x02)
 */
static void emit_rtps_project(int sf, int last);

static void emit_rtps_core(int v, int sf, int lm, int last)
{
    /* Step 1: RT × V + TR → MAC1-3, IR1-3 (inline matrix multiply) */
    emit_inline_mvmva(0, v, 0, sf, lm);

    /* Steps 2-6: SZ push, division, screen projection, SXY push, depth cue */
    emit_rtps_project(sf, last);
}

/* Emit RTPS Steps 2-6: SZ FIFO, perspective division, screen projection,
 * SXY FIFO push, depth cueing.  Assumes MAC1-3 and IR1-3 already stored
 * to cp2_data by the preceding matrix multiply.
 * Clobbers: T8, T9, AT, V0, V1, A0, FPU $f0-$f3, HILO. */
static void emit_rtps_project(int sf, int last)
{
    /* Step 2: Push SZ FIFO inline.
     * SZ3 = saturate_sz( sf ? MAC3 : MAC3>>12 ) */
    EMIT_LW(REG_T8, CPU_CP2_DATA(27), REG_S0);         /* T8 = MAC3 */
    if (!sf)
        EMIT_SRA(REG_T8, REG_T8, 12);                  /* sf=0: need raw>>12 */
    /* Saturate SZ to [0, 0xFFFF] — P19: PMAXW/PMINW */
    EMIT_PMAXW(REG_T8, REG_T8, REG_ZERO);
    EMIT_ORI(REG_T9, REG_ZERO, 0xFFFF);
    EMIT_PMINW(REG_T8, REG_T8, REG_T9);
    /* Shift FIFO */
    EMIT_LW(REG_V0, CPU_CP2_DATA(17), REG_S0);
    EMIT_LW(REG_V1, CPU_CP2_DATA(18), REG_S0);
    EMIT_LW(REG_A0, CPU_CP2_DATA(19), REG_S0);
    EMIT_SW(REG_V0, CPU_CP2_DATA(16), REG_S0);         /* SZ0 = SZ1 */
    EMIT_SW(REG_V1, CPU_CP2_DATA(17), REG_S0);         /* SZ1 = SZ2 */
    EMIT_SW(REG_A0, CPU_CP2_DATA(18), REG_S0);         /* SZ2 = old SZ3 */
    EMIT_SW(REG_T8, CPU_CP2_DATA(19), REG_S0);         /* SZ3 = new */

    /* Step 3: UNR perspective division — fully inline.
     * T8 = SZ3 (saturated).  Compute div_result → A0 */
    EMIT_LH(REG_V0, CPU_CP2_CTRL(26), REG_S0);         /* V0 = (int16)H */
    EMIT_ANDI(REG_V0, REG_V0, 0xFFFF);                  /* V0 = (uint16)H */

    /* Check h < sz3*2 */
    EMIT_SLL(REG_T9, REG_T8, 1);                        /* T9 = sz3 * 2 */
    emit(MK_R(0, REG_V0, REG_T9, REG_V1, 0, 0x2B));    /* SLTU V1, H, sz3*2 */

    /* FPU DIV.S: compute (H * 65536.0f) / SZ3 → A0
     * Uses EE COP1 single-precision float (29-cycle DIV.S latency).
     * Replaces 52-word CLZ16+Newton+table UNR path with 18 words. */
    EMIT_MTC1(REG_T8, 0);                               /* $f0 = SZ3 (int bits) */
    EMIT_CVT_S_W(0, 0);                                 /* $f0 = float(SZ3) */
    EMIT_MTC1(REG_V0, 1);                               /* $f1 = H (int bits) */
    EMIT_CVT_S_W(1, 1);                                 /* $f1 = float(H) */
    EMIT_LUI(REG_AT, 0x4780);                           /* AT = 0x47800000 = 65536.0f */
    EMIT_MTC1(REG_AT, 2);                               /* $f2 = 65536.0f */
    EMIT_MUL_S(1, 1, 2);                                /* $f1 = H * 65536.0 */
    EMIT_DIV_S(1, 1, 0);                                /* $f1 = (H*65536) / SZ3 */
    EMIT_LUI(REG_AT, 0x3F00);                           /* AT = 0x3F000000 = 0.5f */
    EMIT_MTC1(REG_AT, 3);                               /* $f3 = 0.5f */
    EMIT_ADD_S(1, 1, 3);                                /* $f1 += 0.5 (round nearest) */
    EMIT_CVT_W_S(1, 1);                                 /* $f1 = int(trunc toward 0) */
    EMIT_MFC1(REG_A0, 1);                               /* A0 = division result */
    /* Clamp to 0x1FFFF */
    emit_load_imm32(REG_T8, 0x1FFFF);                   /* T8 = 0x1FFFF */
    emit(MK_R(0, REG_T8, REG_A0, REG_AT, 0, 0x2B));    /* SLTU AT, 0x1FFFF, A0 */
    EMIT_MOVN(REG_A0, REG_T8, REG_AT);                  /* if exceeded: clamp */
    /* Handle h >= sz3*2: force 0x1FFFF (V1=0 means h>=sz3*2) */
    EMIT_MOVZ(REG_A0, REG_T8, REG_V1);                  /* A0 = 0x1FFFF if !V1 */

    /* Step 4: Screen projection (32-bit).
     * SX = (div_result * IR1 + OFX) >> 16
     * SY = (div_result * IR2 + OFY) >> 16  */
    EMIT_LH(REG_T8, CPU_CP2_DATA(9), REG_S0);           /* T8 = IR1 */
    EMIT_LW(REG_T9, CPU_CP2_CTRL(24), REG_S0);          /* T9 = OFX */
    EMIT_MULT(REG_A0, REG_T8);                           /* LO = div * IR1 */
    EMIT_MFLO(REG_V0);
    EMIT_ADDU(REG_V0, REG_V0, REG_T9);                   /* V0 = div*IR1 + OFX */
    EMIT_SRA(REG_V0, REG_V0, 16);                        /* V0 = SX */

    EMIT_LH(REG_T8, CPU_CP2_DATA(10), REG_S0);          /* T8 = IR2 */
    EMIT_LW(REG_T9, CPU_CP2_CTRL(25), REG_S0);          /* T9 = OFY */
    EMIT_MULT(REG_A0, REG_T8);                           /* LO = div * IR2 */
    EMIT_MFLO(REG_V1);
    EMIT_ADDU(REG_V1, REG_V1, REG_T9);                   /* V1 = div*IR2 + OFY */
    EMIT_SRA(REG_V1, REG_V1, 16);                        /* V1 = SY */

    /* Step 5: Push SXY FIFO + saturate to [-0x400, 0x3FF] */
    EMIT_LW(REG_T8, CPU_CP2_DATA(13), REG_S0);
    EMIT_LW(REG_T9, CPU_CP2_DATA(14), REG_S0);
    EMIT_SW(REG_T8, CPU_CP2_DATA(12), REG_S0);          /* SXY0 = SXY1 */
    EMIT_SW(REG_T9, CPU_CP2_DATA(13), REG_S0);          /* SXY1 = SXY2 */
    /* Saturate SX/SY to [-0x400, 0x3FF] — P19: PMAXW/PMINW */
    EMIT_ADDIU(REG_T8, REG_ZERO, -0x400);
    EMIT_ORI(REG_T9, REG_ZERO, 0x3FF);
    EMIT_PMAXW(REG_V0, REG_V0, REG_T8);
    EMIT_PMINW(REG_V0, REG_V0, REG_T9);
    EMIT_PMAXW(REG_V1, REG_V1, REG_T8);
    EMIT_PMINW(REG_V1, REG_V1, REG_T9);
    /* Pack SXY2 = (uint16)SX | ((uint16)SY << 16) */
    EMIT_ANDI(REG_V0, REG_V0, 0xFFFF);
    EMIT_SLL(REG_V1, REG_V1, 16);
    EMIT_OR(REG_V0, REG_V0, REG_V1);
    EMIT_SW(REG_V0, CPU_CP2_DATA(14), REG_S0);          /* SXY2 */

    /* Step 6: Depth cueing (last vertex only) */
    if (last) {
        /* MAC0 = DQA * div_result + DQB */
        EMIT_LH(REG_T8, CPU_CP2_CTRL(27), REG_S0);     /* T8 = DQA */
        EMIT_MULT(REG_T8, REG_A0);                      /* LO = DQA * div */
        EMIT_MFLO(REG_T8);
        EMIT_LW(REG_T9, CPU_CP2_CTRL(28), REG_S0);     /* T9 = DQB */
        EMIT_ADDU(REG_T8, REG_T8, REG_T9);              /* T8 = MAC0 */
        EMIT_SW(REG_T8, CPU_CP2_DATA(24), REG_S0);      /* store MAC0 */
        /* IR0 = saturate(MAC0 >> 12, 0, 0x1000) — P19: PMAXW/PMINW */
        EMIT_SRA(REG_T9, REG_T8, 12);
        EMIT_PMAXW(REG_T9, REG_T9, REG_ZERO);
        EMIT_ORI(REG_T8, REG_ZERO, 0x1000);
        EMIT_PMINW(REG_T9, REG_T9, REG_T8);
        EMIT_SW(REG_T9, CPU_CP2_DATA(8), REG_S0);       /* store IR0 */
    }

    /* FLAG=0 */
    EMIT_SW(REG_ZERO, CPU_CP2_CTRL(31), REG_S0);
}

/* ---- Main instruction emitter ---- */
int emit_instruction(uint32_t opcode, uint32_t psx_pc, int *mult_count)
{
    uint32_t op = OP(opcode);
    int rs = RS(opcode);
    int rt = RT(opcode);
    int rd = RD(opcode);
    int sa = SA(opcode);
    int func = FUNC(opcode);
    int16_t imm = SIMM16(opcode);
    uint16_t uimm = IMM16(opcode);

    emit_current_psx_pc = psx_pc;

    if (opcode == 0)
        return 0; /* NOP */

    switch (op)
    {
    case 0x00: /* SPECIAL */
        switch (func)
        {
        case 0x00: /* SLL */
        {
            if (is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rt) << sa);
                break;
            }
            mark_vreg_var(rd);
            int s = emit_use_reg(rt, REG_T8);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, 0, s, d, sa, 0x00));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x02: /* SRL */
        {
            if (is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rt) >> sa);
                break;
            }
            mark_vreg_var(rd);
            int s = emit_use_reg(rt, REG_T8);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, 0, s, d, sa, 0x02));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x03: /* SRA */
        {
            if (is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, (uint32_t)((int32_t)get_vreg_const(rt) >> sa));
                break;
            }
            mark_vreg_var(rd);
            int s = emit_use_reg(rt, REG_T8);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, 0, s, d, sa, 0x03));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x04: /* SLLV */
        {
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rt, REG_T8);
            int s2 = emit_use_reg(rs, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, s2, s1, d, 0, 0x04));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x06: /* SRLV */
        {
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rt, REG_T8);
            int s2 = emit_use_reg(rs, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, s2, s1, d, 0, 0x06));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x07: /* SRAV */
        {
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rt, REG_T8);
            int s2 = emit_use_reg(rs, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, s2, s1, d, 0, 0x07));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x0C: /* SYSCALL */
            emit_load_imm32(REG_A0, psx_pc);
            emit_call_c((uint32_t)Helper_Syscall_Exception);
            emit_block_epilogue();
            return -1;
        case 0x0D: /* BREAK */
            emit_load_imm32(REG_A0, psx_pc);
            emit_call_c((uint32_t)Helper_Break_Exception);
            emit_block_epilogue();
            return -1;
        case 0x10: /* MFHI */
            emit_cpu_field_to_psx_reg(CPU_HI, rd);
            break;
        case 0x11: /* MTHI */
            emit_load_psx_reg(REG_T8, rs);
            EMIT_SW(REG_T8, CPU_HI, REG_S0);
            break;
        case 0x12: /* MFLO */
            emit_cpu_field_to_psx_reg(CPU_LO, rd);
            break;
        case 0x13: /* MTLO */
            emit_load_psx_reg(REG_T8, rs);
            EMIT_SW(REG_T8, CPU_LO, REG_S0);
            break;
        case 0x18: /* MULT */
            emit_load_psx_reg(REG_T8, rs);
            emit_load_psx_reg(REG_T9, rt);
            if (((*mult_count)++ & 1) == 0)
            {
                EMIT_MULT1(REG_T8, REG_T9);
                EMIT_MFLO1(REG_T8);
                EMIT_SW(REG_T8, CPU_LO, REG_S0);
                EMIT_MFHI1(REG_T8);
            }
            else
            {
                emit(MK_R(0, REG_T8, REG_T9, 0, 0, 0x18));
                emit(MK_R(0, 0, 0, REG_T8, 0, 0x12));
                EMIT_SW(REG_T8, CPU_LO, REG_S0);
                emit(MK_R(0, 0, 0, REG_T8, 0, 0x10));
            }
            EMIT_SW(REG_T8, CPU_HI, REG_S0);
            reg_cache_invalidate();
            break;
        case 0x19: /* MULTU */
            emit_load_psx_reg(REG_T8, rs);
            emit_load_psx_reg(REG_T9, rt);
            if (((*mult_count)++ & 1) == 0)
            {
                EMIT_MULTU1(REG_T8, REG_T9);
                EMIT_MFLO1(REG_T8);
                EMIT_SW(REG_T8, CPU_LO, REG_S0);
                EMIT_MFHI1(REG_T8);
            }
            else
            {
                emit(MK_R(0, REG_T8, REG_T9, 0, 0, 0x19));
                emit(MK_R(0, 0, 0, REG_T8, 0, 0x12));
                EMIT_SW(REG_T8, CPU_LO, REG_S0);
                emit(MK_R(0, 0, 0, REG_T8, 0, 0x10));
            }
            EMIT_SW(REG_T8, CPU_HI, REG_S0);
            reg_cache_invalidate();
            break;
        case 0x1A: /* DIV — branchless with MOVZ div-by-zero fixup */
        {
            int s1 = emit_use_reg(rs, REG_T8); /* s1 = EE reg holding rs */
            int s2 = emit_use_reg(rt, REG_T9); /* s2 = EE reg holding rt (divisor) */
            emit(MK_R(0, s1, s2, 0, 0, 0x1A)); /* div s1, s2 */
            /* Fill div latency: compute divz lo = (rs>=0)?-1:1 */
            emit(MK_R(0, 0, s1, REG_AT, 31, 0x03));           /* sra  AT, s1, 31  */
            emit(MK_R(0, 0, REG_AT, REG_AT, 1, 0x00));        /* sll  AT, AT, 1   */
            emit(MK_R(0, REG_AT, REG_ZERO, REG_AT, 0, 0x27)); /* nor  AT, AT, $0  */
            /* AT = divz lo result, s2 still valid (never clobbered) */
            emit(MK_R(0, 0, 0, REG_T8, 0, 0x12)); /* mflo T8 = quotient */
            EMIT_MOVZ(REG_T8, REG_AT, s2);        /* if s2==0: T8 = divz lo */
            EMIT_SW(REG_T8, CPU_LO, REG_S0);
            emit(MK_R(0, 0, 0, REG_T8, 0, 0x10)); /* mfhi T8 = remainder */
            /* divz hi = rs: reload if s1 was clobbered by mflo */
            if (s1 == REG_T8)
            {
                EMIT_LW(REG_AT, CPU_REG(rs), REG_S0);
                EMIT_MOVZ(REG_T8, REG_AT, s2);
            }
            else
            {
                EMIT_MOVZ(REG_T8, s1, s2);
            }
            EMIT_SW(REG_T8, CPU_HI, REG_S0);
            reg_cache_invalidate();
            break;
        }
        case 0x1B: /* DIVU — branchless with MOVZ div-by-zero fixup */
        {
            int s1 = emit_use_reg(rs, REG_T8);    /* s1 = EE reg holding rs */
            int s2 = emit_use_reg(rt, REG_T9);    /* s2 = EE reg holding rt (divisor) */
            emit(MK_R(0, s1, s2, 0, 0, 0x1B));    /* divu s1, s2 */
            EMIT_ADDIU(REG_AT, REG_ZERO, -1);     /* AT = 0xFFFFFFFF (divz lo) */
            emit(MK_R(0, 0, 0, REG_T8, 0, 0x12)); /* mflo T8 = quotient */
            EMIT_MOVZ(REG_T8, REG_AT, s2);        /* if s2==0: T8 = -1 */
            EMIT_SW(REG_T8, CPU_LO, REG_S0);
            emit(MK_R(0, 0, 0, REG_T8, 0, 0x10)); /* mfhi T8 = remainder */
            /* divz hi = rs: reload if s1 was clobbered by mflo */
            if (s1 == REG_T8)
            {
                EMIT_LW(REG_AT, CPU_REG(rs), REG_S0);
                EMIT_MOVZ(REG_T8, REG_AT, s2);
            }
            else
            {
                EMIT_MOVZ(REG_T8, s1, s2);
            }
            EMIT_SW(REG_T8, CPU_HI, REG_S0);
            reg_cache_invalidate();
            break;
        }
        case 0x20: /* ADD — inline overflow detection */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                uint32_t a = get_vreg_const(rs), b = get_vreg_const(rt);
                uint32_t res = a + b;
                if (!((a ^ b) & 0x80000000) && ((res ^ a) & 0x80000000))
                {
                    /* Overflow at compile time — unconditional exception */
                    emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
                    emit_call_c((uint32_t)Helper_Overflow_Exception);
                    emit_abort_check(emit_cycle_offset);
                    break;
                }
                mark_vreg_const_lazy(rd, res);
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);

            /* Flush current rd value to cpu.regs[] so the cold path
             * (overflow) can restore it.  Covers both pinned regs and
             * dirty dynamic slots that haven't been written back yet. */
            if (rd != 0 && psx_pinned_reg[rd])
                EMIT_SW(psx_pinned_reg[rd], CPU_REG(rd), REG_S0);
            else
                dyn_flush_one_slot(rd);

            /* Pre-compute rs^rt; save rs via stack to avoid scratch conflict
             * when rd=0 (emit_dst_reg returns REG_T8 as junk dest). */
            emit(MK_R(0, s1, s2, REG_AT, 0, 0x26)); /* XOR  AT, s1, s2  (rs^rt) */
            EMIT_SW(s1, 76, REG_SP);                /* save rs to stack */

            int d = emit_dst_reg(rd, REG_T8);
            EMIT_ADDU(d, s1, s2); /* d = rs + rt */

            /* Overflow: ~(rs^rt) & (result^rs), bit31=1 => overflow.
             * Load saved rs into scratch (T8 or T9, avoiding d).
             * LW scheduled before NOR to hide load-use delay on R5900. */
            int sr = (d == REG_T8) ? REG_T9 : REG_T8;
            EMIT_LW(sr, 76, REG_SP);                          /* sr = saved rs */
            emit(MK_R(0, REG_AT, REG_ZERO, REG_AT, 0, 0x27)); /* NOR  AT, AT, $0 */
            emit(MK_R(0, d, sr, sr, 0, 0x26));                /* XOR  sr, d, sr  (result^rs) */
            emit(MK_R(0, REG_AT, sr, REG_AT, 0, 0x24));       /* AND  AT, AT, sr */

            /* BGEZ AT, @no_overflow — placeholder, patched after cold path */
            uint32_t *bgez_patch = code_ptr;
            emit(0);
            EMIT_NOP();

            /* ---- Cold path: overflow exception (rarely taken) ---- */
            {
                /* Restore rd from cpu.regs[] (flushed above before ADDU). */
                if (rd != 0 && psx_pinned_reg[rd])
                    EMIT_LW(psx_pinned_reg[rd], CPU_REG(rd), REG_S0);
                else
                    dyn_reload_one_slot(rd);

                RegStatus saved_vregs[32];
                uint32_t saved_dirty = dirty_const_mask;
                uint8_t saved_dyn_dirty = dyn_dirty_mask;
                uint32_t saved_smrv_ov = smrv_known_ram;
                uint32_t saved_align_ov = align_known_mask;
                int saved_t8 = t8_cached_psx_reg, saved_t9 = t9_cached_psx_reg;
                memcpy(saved_vregs, vregs, sizeof(vregs));

                emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
                emit_call_c((uint32_t)Helper_Overflow_Exception);
                emit_abort_check(emit_cycle_offset);

                memcpy(vregs, saved_vregs, sizeof(vregs));
                dirty_const_mask = saved_dirty;
                dyn_dirty_mask = saved_dyn_dirty;
                smrv_known_ram = saved_smrv_ov;
                align_known_mask = saved_align_ov;
                t8_cached_psx_reg = saved_t8;
                t9_cached_psx_reg = saved_t9;
            }

            /* Patch BGEZ: skip from patch_loc+1 to current code_ptr */
            *bgez_patch = MK_I(1, REG_AT, 1, (int16_t)(code_ptr - bgez_patch - 1));

            /* @no_overflow: invalidate scratch cache (XOR/NOR clobbered them) */
            t8_cached_psx_reg = -1;
            t9_cached_psx_reg = -1;
            emit_sync_reg(rd, d);
            break;
        }
        case 0x21: /* ADDU */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rs) + get_vreg_const(rt));
                break;
            }
            /* SMRV: ADDU rd, rs, $0 (MOVE) or ADDU rd, $0, rt propagates RAM-ness */
            int src_ram = (rt == 0 && smrv_is_known_ram(rs)) ||
                          (rs == 0 && smrv_is_known_ram(rt));
            /* Alignment: MOVE propagates alignment */
            int src_aligned = (rt == 0 && align_is_known(rs)) ||
                              (rs == 0 && align_is_known(rt));
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            EMIT_ADDU(d, s1, s2);
            emit_sync_reg(rd, d);
            if (src_ram)
                smrv_set_ram(rd);
            if (src_aligned)
                align_set(rd);
            break;
        }
        case 0x22: /* SUB — inline overflow detection */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                uint32_t a = get_vreg_const(rs), b = get_vreg_const(rt);
                uint32_t res = a - b;
                if (((a ^ b) & 0x80000000) && ((res ^ a) & 0x80000000))
                {
                    emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
                    emit_call_c((uint32_t)Helper_Overflow_Exception);
                    emit_abort_check(emit_cycle_offset);
                    break;
                }
                mark_vreg_const_lazy(rd, res);
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);

            /* Flush pinned/dynamic rd so cold path can restore from cpu.regs[] */
            if (rd != 0 && psx_pinned_reg[rd])
                EMIT_SW(psx_pinned_reg[rd], CPU_REG(rd), REG_S0);
            else
                dyn_flush_one_slot(rd);

            /* Pre-compute rs^rt; save rs via stack */
            emit(MK_R(0, s1, s2, REG_AT, 0, 0x26)); /* XOR  AT, s1, s2 */
            EMIT_SW(s1, 76, REG_SP);                /* save rs to stack */

            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, s1, s2, d, 0, 0x23)); /* SUBU d, s1, s2 */

            /* Overflow: (rs^rt) & (result^rs), bit31=1 => overflow */
            int sr = (d == REG_T8) ? REG_T9 : REG_T8;
            EMIT_LW(sr, 76, REG_SP);                    /* sr = saved rs */
            EMIT_NOP();                                 /* load delay */
            emit(MK_R(0, d, sr, sr, 0, 0x26));          /* XOR  sr, d, sr */
            emit(MK_R(0, REG_AT, sr, REG_AT, 0, 0x24)); /* AND  AT, AT, sr */

            uint32_t *bgez_patch = code_ptr;
            emit(0);
            EMIT_NOP();

            {
                if (rd != 0 && psx_pinned_reg[rd])
                    EMIT_LW(psx_pinned_reg[rd], CPU_REG(rd), REG_S0);
                else
                    dyn_reload_one_slot(rd);

                RegStatus saved_vregs[32];
                uint32_t saved_dirty = dirty_const_mask;
                uint8_t saved_dyn_dirty = dyn_dirty_mask;
                uint32_t saved_smrv_ov = smrv_known_ram;
                uint32_t saved_align_ov = align_known_mask;
                int saved_t8 = t8_cached_psx_reg, saved_t9 = t9_cached_psx_reg;
                memcpy(saved_vregs, vregs, sizeof(vregs));

                emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
                emit_call_c((uint32_t)Helper_Overflow_Exception);
                emit_abort_check(emit_cycle_offset);

                memcpy(vregs, saved_vregs, sizeof(vregs));
                dirty_const_mask = saved_dirty;
                dyn_dirty_mask = saved_dyn_dirty;
                smrv_known_ram = saved_smrv_ov;
                align_known_mask = saved_align_ov;
                t8_cached_psx_reg = saved_t8;
                t9_cached_psx_reg = saved_t9;
            }

            *bgez_patch = MK_I(1, REG_AT, 1, (int16_t)(code_ptr - bgez_patch - 1));

            t8_cached_psx_reg = -1;
            t9_cached_psx_reg = -1;
            emit_sync_reg(rd, d);
            break;
        }
        case 0x23: /* SUBU */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rs) - get_vreg_const(rt));
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, s1, s2, d, 0, 0x23));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x24: /* AND */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rs) & get_vreg_const(rt));
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, s1, s2, d, 0, 0x24));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x25: /* OR */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rs) | get_vreg_const(rt));
                break;
            }
            /* SMRV: OR rd, rs, $0 (MOVE) propagates RAM-ness */
            int src_ram = (rt == 0 && smrv_is_known_ram(rs)) ||
                          (rs == 0 && smrv_is_known_ram(rt));
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            EMIT_OR(d, s1, s2);
            emit_sync_reg(rd, d);
            if (src_ram)
                smrv_set_ram(rd);
            break;
        }
        case 0x26: /* XOR */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rs) ^ get_vreg_const(rt));
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, s1, s2, d, 0, 0x26));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x27: /* NOR */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, ~(get_vreg_const(rs) | get_vreg_const(rt)));
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, s1, s2, d, 0, 0x27));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x2A: /* SLT */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, ((int32_t)get_vreg_const(rs) < (int32_t)get_vreg_const(rt)) ? 1 : 0);
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, s1, s2, d, 0, 0x2A));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x2B: /* SLTU */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, (get_vreg_const(rs) < get_vreg_const(rt)) ? 1 : 0);
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, s1, s2, d, 0, 0x2B));
            emit_sync_reg(rd, d);
            break;
        }
        default:
            if (total_instructions < 50)
                DLOG("Unknown SPECIAL func=0x%02X at PC=0x%08X\n", func, (unsigned)psx_pc);
            break;
        }
        break;

    /* I-type ALU */
    case 0x08: /* ADDI — inline overflow detection */
    {
        if (is_vreg_const(rs))
        {
            uint32_t a = get_vreg_const(rs);
            uint32_t b = (uint32_t)imm; /* sign-extended immediate */
            uint32_t res = a + b;
            if (!((a ^ b) & 0x80000000) && ((res ^ a) & 0x80000000))
            {
                emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
                emit_call_c((uint32_t)Helper_Overflow_Exception);
                emit_abort_check(emit_cycle_offset);
                break;
            }
            mark_vreg_const_lazy(rt, res);
            break;
        }
        int rs_ram = smrv_is_known_ram(rs);
        int rs_aligned = align_is_known(rs);
        mark_vreg_var(rt);
        int s1 = emit_use_reg(rs, REG_T8);

        /* Flush pinned/dynamic rt so cold path can restore from cpu.regs[] */
        if (rt != 0 && psx_pinned_reg[rt])
            EMIT_SW(psx_pinned_reg[rt], CPU_REG(rt), REG_S0);
        else
            dyn_flush_one_slot(rt);

        /* Load sign-extended immediate into T1 for overflow check */
        emit_load_imm32(REG_T9, (uint32_t)imm);

        /* Pre-compute rs^imm; save rs via stack */
        emit(MK_R(0, s1, REG_T9, REG_AT, 0, 0x26)); /* XOR  AT, s1, T1 */
        EMIT_SW(s1, 76, REG_SP);                    /* save rs to stack */

        int d = emit_dst_reg(rt, REG_T8);
        EMIT_ADDU(d, s1, REG_T9); /* d = rs + imm */

        /* Overflow: ~(rs^imm) & (result^rs), bit31=1 => overflow */
        int sr = (d == REG_T8) ? REG_T9 : REG_T8;
        EMIT_LW(sr, 76, REG_SP);                          /* sr = saved rs */
        emit(MK_R(0, REG_AT, REG_ZERO, REG_AT, 0, 0x27)); /* NOR  AT, AT, $0 */
        emit(MK_R(0, d, sr, sr, 0, 0x26));                /* XOR  sr, d, sr */
        emit(MK_R(0, REG_AT, sr, REG_AT, 0, 0x24));       /* AND  AT, AT, sr */

        uint32_t *bgez_patch = code_ptr;
        emit(0);
        EMIT_NOP();

        {
            if (rt != 0 && psx_pinned_reg[rt])
                EMIT_LW(psx_pinned_reg[rt], CPU_REG(rt), REG_S0);
            else
                dyn_reload_one_slot(rt);

            RegStatus saved_vregs[32];
            uint32_t saved_dirty = dirty_const_mask;
            uint8_t saved_dyn_dirty = dyn_dirty_mask;
            uint32_t saved_smrv_ov = smrv_known_ram;
            uint32_t saved_align_ov = align_known_mask;
            int saved_t8 = t8_cached_psx_reg, saved_t9 = t9_cached_psx_reg;
            memcpy(saved_vregs, vregs, sizeof(vregs));

            emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
            emit_call_c((uint32_t)Helper_Overflow_Exception);
            emit_abort_check(emit_cycle_offset);

            memcpy(vregs, saved_vregs, sizeof(vregs));
            dirty_const_mask = saved_dirty;
            dyn_dirty_mask = saved_dyn_dirty;
            smrv_known_ram = saved_smrv_ov;
            align_known_mask = saved_align_ov;
            t8_cached_psx_reg = saved_t8;
            t9_cached_psx_reg = saved_t9;
        }

        *bgez_patch = MK_I(1, REG_AT, 1, (int16_t)(code_ptr - bgez_patch - 1));

        t8_cached_psx_reg = -1;
        t9_cached_psx_reg = -1;
        emit_sync_reg(rt, d);
        /* SMRV: ADDI from RAM base stays in RAM (if no overflow) */
        if (rs_ram)
            smrv_set_ram(rt);
        /* Alignment: ADDI preserves alignment when offset is word-aligned */
        if (rs_aligned && (imm & 3) == 0)
            align_set(rt);
        break;
    }
    case 0x09: /* ADDIU */
    {
        if (is_vreg_const(rs))
        {
            mark_vreg_const_lazy(rt, get_vreg_const(rs) + imm);
            break;
        }
        int rs_ram = smrv_is_known_ram(rs);
        int rs_aligned = align_is_known(rs);
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T8);
        int d = emit_dst_reg(rt, REG_T8);
        EMIT_ADDIU(d, s, imm);
        emit_sync_reg(rt, d);
        /* SMRV: ADDIU from RAM base with small offset stays in RAM */
        if (rs_ram)
            smrv_set_ram(rt);
        /* Alignment: ADDIU preserves alignment when offset is word-aligned */
        if (rs_aligned && (imm & 3) == 0)
            align_set(rt);
        break;
    }
    case 0x0A: /* SLTI */
    {
        if (is_vreg_const(rs))
        {
            mark_vreg_const_lazy(rt, ((int32_t)get_vreg_const(rs) < (int32_t)imm) ? 1 : 0);
            break;
        }
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T8);
        int d = emit_dst_reg(rt, REG_T8);
        emit(MK_I(0x0A, s, d, imm));
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0B: /* SLTIU */
    {
        if (is_vreg_const(rs))
        {
            mark_vreg_const_lazy(rt, (get_vreg_const(rs) < (uint32_t)(int32_t)imm) ? 1 : 0);
            break;
        }
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T8);
        int d = emit_dst_reg(rt, REG_T8);
        emit(MK_I(0x0B, s, d, imm));
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0C: /* ANDI */
    {
        if (is_vreg_const(rs))
        {
            mark_vreg_const_lazy(rt, get_vreg_const(rs) & uimm);
            break;
        }
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T8);
        int d = emit_dst_reg(rt, REG_T8);
        emit(MK_I(0x0C, s, d, uimm));
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0D: /* ORI */
    {
        if (is_vreg_const(rs))
        {
            mark_vreg_const_lazy(rt, get_vreg_const(rs) | uimm);
            break;
        }
        int rs_ram = smrv_is_known_ram(rs);
        int rs_aligned = align_is_known(rs);
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T8);
        int d = emit_dst_reg(rt, REG_T8);
        EMIT_ORI(d, s, uimm);
        emit_sync_reg(rt, d);
        /* SMRV: ORI from RAM base (e.g., LUI+ORI pattern) stays in RAM */
        if (rs_ram)
            smrv_set_ram(rt);
        /* Alignment: ORI preserves alignment when low bits don't break it */
        if (rs_aligned && (uimm & 3) == 0)
            align_set(rt);
        break;
    }
    case 0x0E: /* XORI */
    {
        if (is_vreg_const(rs))
        {
            mark_vreg_const_lazy(rt, get_vreg_const(rs) ^ uimm);
            break;
        }
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T8);
        int d = emit_dst_reg(rt, REG_T8);
        emit(MK_I(0x0E, s, d, uimm));
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0F: /* LUI */
    {
        mark_vreg_const_lazy(rt, (uint32_t)uimm << 16);
        break;
    }

    /* COP0 */
    case 0x10:
    {
        if (rs == 0x00)
        {
            /* MFC0 rt, rd */
            emit_cpu_field_to_psx_reg(CPU_COP0(rd), rt);
        }
        else if (rs == 0x04)
        {
            /* MTC0 rt, rd */
            emit_load_psx_reg(REG_T8, rt);
            if (rd == PSX_COP0_SR)
            {
                EMIT_MOVE(REG_A0, REG_T8);
                emit_call_c((uint32_t)debug_mtc0_sr);
            }
            else
            {
                EMIT_SW(REG_T8, CPU_COP0(rd), REG_S0);
            }
        }
        else if (rs == 0x10 && func == 0x10)
        {
            /* RFE */
            reg_cache_invalidate();
            EMIT_LW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
            EMIT_MOVE(REG_T9, REG_T8);
            emit(MK_R(0, 0, REG_T9, REG_T9, 2, 0x02));
            emit(MK_I(0x0C, REG_T9, REG_T9, 0x0F));
            emit(MK_R(0, 0, REG_T8, REG_T8, 4, 0x02));
            emit(MK_R(0, 0, REG_T8, REG_T8, 4, 0x00));
            EMIT_OR(REG_T8, REG_T8, REG_T9);
            EMIT_SW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
        }
        break;
    }

    /* COP1 */
    case 0x11:
    {
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 1);
        reg_cache_invalidate();
        EMIT_LW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T8, REG_T8, 29, 0x02));
        emit(MK_I(0x0C, REG_T8, REG_T8, 1));
        uint32_t *skip_patch_1 = code_ptr;
        emit(MK_I(0x05, REG_T8, REG_ZERO, 0));
        EMIT_NOP();
        {
            uint8_t saved_dirty = dyn_dirty_mask;
            uint32_t saved_align_cu = align_known_mask;
            emit_call_c((uint32_t)Helper_CU_Exception);
            dyn_dirty_mask = saved_dirty;
            align_known_mask = saved_align_cu;
        }
        *skip_patch_1 = (*skip_patch_1 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_patch_1 - 1) & 0xFFFF);
        break;
    }

    /* COP2 (GTE) */
    case 0x12:
    {
        if (!block_cu2_hoisted)
        {
            flush_dirty_consts(); /* Flush before COP-usable conditional */
            reg_cache_invalidate();
            EMIT_LW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
            emit(MK_R(0, 0, REG_T8, REG_T8, 30, 0x02));
            emit(MK_I(0x0C, REG_T8, REG_T8, 1));
            uint32_t *skip_cu2 = code_ptr;
            emit(MK_I(0x05, REG_T8, REG_ZERO, 0));
            EMIT_NOP();
            emit_load_imm32(REG_A0, psx_pc);
            emit_load_imm32(REG_A1, 2);
            {
                uint8_t saved_dirty = dyn_dirty_mask;
                uint32_t saved_align_cu = align_known_mask;
                emit_call_c((uint32_t)Helper_CU_Exception);
                dyn_dirty_mask = saved_dirty;
                align_known_mask = saved_align_cu;
            }
            *skip_cu2 = (*skip_cu2 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_cu2 - 1) & 0xFFFF);
        }

        if (total_instructions < 20000000)
        {
            DLOG("Compiling COP2 Op %08" PRIX32 " at %08" PRIX32 "\n", opcode, psx_pc);
        }
        if ((opcode & 0x02000000) == 0)
        {
            if (rs == 0x00)
            {
                mark_vreg_var(rt);
                if (rd == 15 || rd == 28 || rd == 29)
                {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, rd);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_ReadData);
                    emit_store_psx_reg(rt, REG_V0);
                }
                else
                {
                    EMIT_LW(REG_V0, CPU_CP2_DATA(rd & 0x1F), REG_S0);
                    emit_store_psx_reg(rt, REG_V0);
                }
            }
            else if (rs == 0x02)
            {
                mark_vreg_var(rt);
                if (rd == 31)
                {
                    /* CFC2 $rt, $31 — FLAG register read.
                     * Call GTE_ReadCtrl so flag-read detection works
                     * (for VU0 fast-path gating). */
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, 31);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_ReadCtrl);
                    emit_store_psx_reg(rt, REG_V0);
                }
                else
                {
                    EMIT_LW(REG_V0, CPU_CP2_CTRL(rd & 0x1F), REG_S0);
                    emit_store_psx_reg(rt, REG_V0);
                }
            }
            else if (rs == 0x04)
            {
                /* MTC2: write PSX reg rt → GTE data register rd.
                 * Inline simple writes to avoid emit_call_c_lite overhead.
                 * Complex regs (15=SXY FIFO, 28=IRGB, 30=LZCS) keep C call. */
                int rd5 = rd & 0x1F;
                switch (rd5)
                {
                case 15:
                case 28:
                case 30:
                    /* Complex: SXY FIFO push / IRGB / LZCS+LZCR */
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, rd5);
                    emit_load_psx_reg(6, rt);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_WriteData);
                    break;
                case 29:
                case 31:
                    /* Read-only: no-op */
                    break;
                case 1:
                case 3:
                case 5:
                case 8:
                case 9:
                case 10:
                case 11:
                    /* Sign-extend from low 16 bits: D(r) = (int32_t)(int16_t)val */
                    emit_load_psx_reg(REG_T8, rt);
                    emit(MK_R(0, 0, REG_T8, REG_T8, 16, 0x00)); /* sll t8, t8, 16  */
                    emit(MK_R(0, 0, REG_T8, REG_T8, 16, 0x03)); /* sra t8, t8, 16  */
                    EMIT_SW(REG_T8, CPU_CP2_DATA(rd5), REG_S0);
                    break;
                case 7:
                case 16:
                case 17:
                case 18:
                case 19:
                    /* Zero-extend to 16 bits: D(r) = val & 0xFFFF */
                    emit_load_psx_reg(REG_T8, rt);
                    emit(MK_I(0x0C, REG_T8, REG_T8, 0xFFFF)); /* andi t8, 0xFFFF */
                    EMIT_SW(REG_T8, CPU_CP2_DATA(rd5), REG_S0);
                    break;
                default:
                    /* Simple write: D(r) = val */
                    emit_load_psx_reg(REG_T8, rt);
                    EMIT_SW(REG_T8, CPU_CP2_DATA(rd5), REG_S0);
                    break;
                }
            }
            else if (rs == 0x06)
            {
                /* CTC2: write PSX reg rt → GTE control register rd.
                 * Inline simple writes; keep C call for FLAG (rd=31). */
                int rd5 = rd & 0x1F;
                switch (rd5)
                {
                case 31:
                    /* FLAG: complex bit-check logic */
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, rd5);
                    emit_load_psx_reg(6, rt);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_WriteCtrl);
                    break;
                case 4:
                case 12:
                case 20:
                case 26:
                case 27:
                case 29:
                case 30:
                    /* Sign-extend from low 16 bits */
                    emit_load_psx_reg(REG_T8, rt);
                    emit(MK_R(0, 0, REG_T8, REG_T8, 16, 0x00)); /* sll t8, 16 */
                    emit(MK_R(0, 0, REG_T8, REG_T8, 16, 0x03)); /* sra t8, 16 */
                    EMIT_SW(REG_T8, CPU_CP2_CTRL(rd5), REG_S0);
                    break;
                default:
                    /* Simple write: C(r) = val */
                    emit_load_psx_reg(REG_T8, rt);
                    EMIT_SW(REG_T8, CPU_CP2_CTRL(rd5), REG_S0);
                    break;
                }
            }
            else
            {
                if (total_instructions < 100)
                    DLOG("Unknown COP2 transfer rs=0x%X\n", rs);
            }
        }
        else
        {
            uint32_t gte_func = opcode & 0x3F;
            int gte_sf = (opcode >> 19) & 1;
            int gte_lm = (opcode >> 10) & 1;
            switch (gte_func)
            {
            case 0x01: /* RTPS */
                if (gte_use_vu0 && gte_sf) {
                    emit_rtps_core(0, gte_sf, gte_lm, 1);
                } else {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_RTPS);
                }
                break;
            case 0x06: /* NCLIP */
                if (gte_use_vu0)
                {
                    /* ---- Inline NCLIP (fast path) ----
                     * MAC0 = SX0*(SY1-SY2) + SX1*(SY2-SY0) + SX2*(SY0-SY1)
                     * FLAG = 0 (no overflow for typical screen coords)
                     *
                     * Uses LH to directly extract sign-extended int16 X/Y
                     * from packed SXY registers (little-endian: X=lo16, Y=hi16).
                     * Scratch: T8/T9/AT (SX), A0-A2 (SY then diffs), V0-V1 (diffs)
                     * Uses EE pipeline 0 MULT/MADD/MFLO
                     */

                    /* Load 6 halfwords: SX0,SY0, SX1,SY1, SX2,SY2 */
                    EMIT_LH(REG_T8, CPU_CP2_DATA(12) + 0, REG_S0); /* T8 = SX0 */
                    EMIT_LH(REG_A0, CPU_CP2_DATA(12) + 2, REG_S0); /* A0 = SY0 */
                    EMIT_LH(REG_T9, CPU_CP2_DATA(13) + 0, REG_S0); /* T9 = SX1 */
                    EMIT_LH(REG_A1, CPU_CP2_DATA(13) + 2, REG_S0); /* A1 = SY1 */
                    EMIT_LH(REG_AT, CPU_CP2_DATA(14) + 0, REG_S0); /* AT = SX2 */
                    EMIT_LH(REG_A2, CPU_CP2_DATA(14) + 2, REG_S0); /* A2 = SY2 */

                    /* Y differences */
                    EMIT_SUBU(REG_V0, REG_A1, REG_A2);             /* V0 = SY1-SY2 */
                    EMIT_SUBU(REG_V1, REG_A2, REG_A0);             /* V1 = SY2-SY0 */
                    EMIT_SUBU(REG_A0, REG_A0, REG_A1);             /* A0 = SY0-SY1 */

                    /* Multiply + accumulate */
                    EMIT_MULT(REG_T8, REG_V0);                     /* LO = SX0*(SY1-SY2) */
                    EMIT_MADD(REG_T9, REG_V1);                     /* LO += SX1*(SY2-SY0) */
                    EMIT_MADD(REG_AT, REG_A0);                     /* LO += SX2*(SY0-SY1) */
                    EMIT_MFLO(REG_V0);                              /* V0 = MAC0 */

                    /* Store results */
                    EMIT_SW(REG_V0, CPU_CP2_DATA(24), REG_S0);     /* MAC0 */
                    EMIT_SW(REG_ZERO, CPU_CP2_CTRL(31), REG_S0);   /* FLAG = 0 */
                }
                else
                {
                    /* Exact C path — full 64-bit overflow detection */
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_NCLIP);
                }
                break;
            case 0x0C: /* OP */
                if (gte_use_vu0)
                {
                    /* ---- Inline OP (fast path) ----
                     * Cross product: D × IR
                     * MAC1 = IR3*D2 - IR2*D3
                     * MAC2 = IR1*D3 - IR3*D1
                     * MAC3 = IR2*D1 - IR1*D2
                     * D1=lo16(RT11RT12), D2=lo16(RT22RT23), D3=lo16(RT33)
                     * Then store_mac_ir (sf shift + IR saturation)
                     * FLAG = 0
                     *
                     * Scratch: T8(D1),T9(D2),AT(D3), V0(IR1),V1(IR2),A0(IR3)
                     *          A1-A3(MAC/temps)
                     */

                    /* Load rotation diagonal D1,D2,D3 (lo16 = signed halfword) */
                    EMIT_LH(REG_T8, CPU_CP2_CTRL(0),  REG_S0);     /* T8 = D1 = lo16(RT11RT12) */
                    EMIT_LH(REG_T9, CPU_CP2_CTRL(2),  REG_S0);     /* T9 = D2 = lo16(RT22RT23) */
                    EMIT_LH(REG_AT, CPU_CP2_CTRL(4),  REG_S0);     /* AT = D3 = lo16(RT33) */

                    /* Load IR1-3 */
                    EMIT_LH(REG_V0, CPU_CP2_DATA(9),  REG_S0);     /* V0 = IR1 */
                    EMIT_LH(REG_V1, CPU_CP2_DATA(10), REG_S0);     /* V1 = IR2 */
                    EMIT_LH(REG_A0, CPU_CP2_DATA(11), REG_S0);     /* A0 = IR3 */

                    /* MAC1 = IR3*D2 - IR2*D3 */
                    EMIT_MULT(REG_A0, REG_T9);                     /* LO = IR3*D2 */
                    EMIT_MFLO(REG_A1);                              /* A1 = IR3*D2 */
                    EMIT_MULT(REG_V1, REG_AT);                     /* LO = IR2*D3 */
                    EMIT_MFLO(REG_A2);                              /* A2 = IR2*D3 */
                    EMIT_SUBU(REG_A1, REG_A1, REG_A2);             /* A1 = MAC1 */

                    /* MAC2 = IR1*D3 - IR3*D1 */
                    EMIT_MULT(REG_V0, REG_AT);                     /* LO = IR1*D3 */
                    EMIT_MFLO(REG_A2);                              /* A2 = IR1*D3 */
                    EMIT_MULT(REG_A0, REG_T8);                     /* LO = IR3*D1 */
                    EMIT_MFLO(REG_A3);                              /* A3 = IR3*D1 */
                    EMIT_SUBU(REG_A2, REG_A2, REG_A3);             /* A2 = MAC2 */

                    /* MAC3 = IR2*D1 - IR1*D2 */
                    EMIT_MULT(REG_V1, REG_T8);                     /* LO = IR2*D1 */
                    EMIT_MFLO(REG_A3);                              /* A3 = IR2*D1 */
                    EMIT_MULT(REG_V0, REG_T9);                     /* LO = IR1*D2 */
                    EMIT_MFLO(REG_V0);                              /* V0 = IR1*D2 (reuse) */
                    EMIT_SUBU(REG_A3, REG_A3, REG_V0);             /* A3 = MAC3 */

                    /* sf=1: shift right by 12 */
                    if (gte_sf) {
                        EMIT_SRA(REG_A1, REG_A1, 12);
                        EMIT_SRA(REG_A2, REG_A2, 12);
                        EMIT_SRA(REG_A3, REG_A3, 12);
                    }

                    /* Store MAC1-3 */
                    EMIT_SW(REG_A1, CPU_CP2_DATA(25), REG_S0);     /* MAC1 */
                    EMIT_SW(REG_A2, CPU_CP2_DATA(26), REG_S0);     /* MAC2 */
                    EMIT_SW(REG_A3, CPU_CP2_DATA(27), REG_S0);     /* MAC3 */

                    /* Saturate IR: clamp [lo..0x7FFF] — P19: PMAXW/PMINW */
                    EMIT_ORI(REG_T9, REG_ZERO, 0x7FFF);
                    if (gte_lm) {
                        EMIT_PMAXW(REG_A1, REG_A1, REG_ZERO);
                        EMIT_PMINW(REG_A1, REG_A1, REG_T9);
                        EMIT_PMAXW(REG_A2, REG_A2, REG_ZERO);
                        EMIT_PMINW(REG_A2, REG_A2, REG_T9);
                        EMIT_PMAXW(REG_A3, REG_A3, REG_ZERO);
                        EMIT_PMINW(REG_A3, REG_A3, REG_T9);
                    } else {
                        EMIT_ADDIU(REG_T8, REG_ZERO, -0x8000);
                        EMIT_PMAXW(REG_A1, REG_A1, REG_T8);
                        EMIT_PMINW(REG_A1, REG_A1, REG_T9);
                        EMIT_PMAXW(REG_A2, REG_A2, REG_T8);
                        EMIT_PMINW(REG_A2, REG_A2, REG_T9);
                        EMIT_PMAXW(REG_A3, REG_A3, REG_T8);
                        EMIT_PMINW(REG_A3, REG_A3, REG_T9);
                    }

                    /* Store IR1-3 + FLAG=0 */
                    EMIT_SW(REG_A1, CPU_CP2_DATA(9),  REG_S0);     /* IR1 */
                    EMIT_SW(REG_A2, CPU_CP2_DATA(10), REG_S0);     /* IR2 */
                    EMIT_SW(REG_A3, CPU_CP2_DATA(11), REG_S0);     /* IR3 */
                    EMIT_SW(REG_ZERO, CPU_CP2_CTRL(31), REG_S0);   /* FLAG = 0 */
                    /* Total: sf=0/lm=1: 41w, sf=1/lm=0: 45w */
                }
                else
                {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_OP);
                }
                break;
            case 0x10: /* DPCS */
                if (gte_use_vu0)
                {
                    /* ---- Inline DPCS (fast path) ----
                     * acc = [R,G,B] << 16 from RGBC
                     * Then interpolate_color_acc + push_color
                     */

                    /* Extract R,G,B from RGBC and shift << 16 */
                    EMIT_LW(REG_T8, CPU_CP2_DATA(6), REG_S0);      /* T8 = RGBC */
                    EMIT_ANDI(REG_V0, REG_T8, 0xFF);               /* V0 = R */
                    emit(MK_R(0, 0, REG_T8, REG_T9, 8, 0x02));    /* SRL T9,T8,8 */
                    EMIT_ANDI(REG_V1, REG_T9, 0xFF);               /* V1 = G */
                    emit(MK_R(0, 0, REG_T8, REG_T9, 16, 0x02));   /* SRL T9,T8,16 */
                    EMIT_ANDI(REG_A0, REG_T9, 0xFF);               /* A0 = B */
                    EMIT_SLL(REG_V0, REG_V0, 16);                  /* V0 = R<<16 */
                    EMIT_SLL(REG_V1, REG_V1, 16);                  /* V1 = G<<16 */
                    EMIT_SLL(REG_A0, REG_A0, 16);                  /* A0 = B<<16 */

                    emit_interpolate_color(gte_sf);
                    emit_push_color_inline();
                    emit_ir_sat_store(gte_lm);
                }
                else
                {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_DPCS);
                }
                break;
            case 0x11: /* INTPL */
                if (gte_use_vu0)
                {
                    /* ---- Inline INTPL (fast path) ----
                     * acc = [IR1,IR2,IR3] << 12
                     * Then interpolate_color_acc + push_color
                     */

                    /* Load IR1-3 and shift << 12 */
                    EMIT_LH(REG_V0, CPU_CP2_DATA(9),  REG_S0);    /* V0 = IR1 */
                    EMIT_LH(REG_V1, CPU_CP2_DATA(10), REG_S0);    /* V1 = IR2 */
                    EMIT_LH(REG_A0, CPU_CP2_DATA(11), REG_S0);    /* A0 = IR3 */
                    EMIT_SLL(REG_V0, REG_V0, 12);                  /* V0 = IR1<<12 */
                    EMIT_SLL(REG_V1, REG_V1, 12);                  /* V1 = IR2<<12 */
                    EMIT_SLL(REG_A0, REG_A0, 12);                  /* A0 = IR3<<12 */

                    emit_interpolate_color(gte_sf);
                    emit_push_color_inline();
                    emit_ir_sat_store(gte_lm);
                }
                else
                {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_INTPL);
                }
                break;
            case 0x12: /* MVMVA */
            {
                int mx = (opcode >> 17) & 3;
                int v = (opcode >> 15) & 3;
                int cv = (opcode >> 13) & 3;
                if (gte_use_vu0 && gte_sf && mx < 3 && cv != 2) {
                    /* Inline: mx=0(RT)/1(L)/2(LC), v=0-3, cv=0(TR)/1(BK)/3(none) */
                    emit_inline_mvmva(mx, v, cv, gte_sf, gte_lm);
                } else {
                    /* Bugged paths (mx=3, cv=2) or vu0 disabled → C fallback */
                    uint32_t packed = gte_sf | (gte_lm << 1) | (mx << 2) | (v << 4) | (cv << 6);
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, packed);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_MVMVA);
                }
                break;
            }
            case 0x13: /* NCDS */
                if (gte_use_vu0 && gte_sf) {
                    emit_ncds_core(0, gte_sf, gte_lm);
                } else {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_NCDS);
                }
                break;
            case 0x14: /* CDP */
                if (gte_use_vu0 && gte_sf) {
                    /* BK + Color × IR → RGBC×IR<<4 → interpolate + push_color */
                    emit_inline_mvmva(2, 3, 1, gte_sf, gte_lm);
                    emit_rgbc_times_ir_shl4();
                    emit_interpolate_color(gte_sf);
                    emit_push_color_inline();
                    emit_ir_sat_store(gte_lm);
                } else {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_CDP);
                }
                break;
            case 0x16: /* NCDT */
                if (gte_use_vu0 && gte_sf) {
#ifdef ENABLE_VU0_MICRO
                    /* P21: Overlapped ×3 — hide VU0 Light×V behind EE post-lighting. */
                    /* Vertex 0: full sync */
                    emit_vu0_micro_prepare(1, 3);
                    emit_vu0_micro_multiply(0, gte_lm, 0);
                    emit_vu0_micro_prepare(2, 1);
                    emit_vu0_micro_multiply(3, gte_lm, 0);
                    /* Overlap: launch Light×V1, post-light V0 */
                    emit_vu0_micro_prepare(1, 3);
                    emit_vu0_micro_launch(1, 0);
                    emit_ncds_post_lighting(gte_sf, gte_lm);
                    emit_vu0_micro_poll_complete(gte_lm);
                    /* Vertex 1: Color×IR sync */
                    emit_vu0_micro_prepare(2, 1);
                    emit_vu0_micro_multiply(3, gte_lm, 0);
                    /* Overlap: launch Light×V2, post-light V1 */
                    emit_vu0_micro_prepare(1, 3);
                    emit_vu0_micro_launch(2, 0);
                    emit_ncds_post_lighting(gte_sf, gte_lm);
                    emit_vu0_micro_poll_complete(gte_lm);
                    /* Vertex 2: Color×IR sync + final post-light */
                    emit_vu0_micro_prepare(2, 1);
                    emit_vu0_micro_multiply(3, gte_lm, 0);
                    emit_ncds_post_lighting(gte_sf, gte_lm);
#elif defined(PLATFORM_PS2)
                    /* P18 macro: preload L(VF1-4) + LC(VF7-10) once. */
                    if (gte_sf) {
                        emit_vu0_load_matrix(1, 3, 1);
                        emit_vu0_load_matrix(2, 1, 7);
                        vu0_preloaded[1] = 1;
                        vu0_preloaded[2] = 7;
                    }
                    emit_ncds_core(0, gte_sf, gte_lm);
                    emit_ncds_core(1, gte_sf, gte_lm);
                    emit_ncds_core(2, gte_sf, gte_lm);
                    vu0_preloaded[1] = 0;
                    vu0_preloaded[2] = 0;
#endif
                } else {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_NCDT);
                }
                break;
            case 0x1B: /* NCCS */
                if (gte_use_vu0 && gte_sf) {
                    emit_nccs_core(0, gte_sf, gte_lm);
                } else {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_NCCS);
                }
                break;
            case 0x1C: /* CC */
                if (gte_use_vu0 && gte_sf) {
                    /* BK + Color × IR → RGBC×IR<<4 → store_mac_ir + push_color */
                    emit_inline_mvmva(2, 3, 1, gte_sf, gte_lm);
                    emit_rgbc_times_ir_shl4();
                    emit_sf_mac_push_ir(gte_sf, gte_lm);
                } else {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_CC);
                }
                break;
            case 0x1E: /* NCS */
                if (gte_use_vu0 && gte_sf) {
                    emit_ncs_core(0, gte_sf, gte_lm);
                } else {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_NCS);
                }
                break;
            case 0x20: /* NCT */
                if (gte_use_vu0 && gte_sf) {
#ifdef ENABLE_VU0_MICRO
                    /* P21: Overlapped ×3 — hide VU0 Light×V behind EE post-lighting.
                     * V0: sync both mvmvas → launch V1 Light → post_light V0 → poll
                     * V1: sync Color×IR    → launch V2 Light → post_light V1 → poll
                     * V2: sync Color×IR    → post_light V2 */
                    /* Vertex 0: full sync */
                    emit_vu0_micro_prepare(1, 3);
                    emit_vu0_micro_multiply(0, gte_lm, 0);
                    emit_vu0_micro_prepare(2, 1);
                    emit_vu0_micro_multiply(3, gte_lm, 0);
                    /* Overlap: launch Light×V1, post-light V0 */
                    emit_vu0_micro_prepare(1, 3);
                    emit_vu0_micro_launch(1, 0);
                    emit_ncs_post_lighting();
                    emit_vu0_micro_poll_complete(gte_lm);
                    /* Vertex 1: Color×IR sync */
                    emit_vu0_micro_prepare(2, 1);
                    emit_vu0_micro_multiply(3, gte_lm, 0);
                    /* Overlap: launch Light×V2, post-light V1 */
                    emit_vu0_micro_prepare(1, 3);
                    emit_vu0_micro_launch(2, 0);
                    emit_ncs_post_lighting();
                    emit_vu0_micro_poll_complete(gte_lm);
                    /* Vertex 2: Color×IR sync + final post-light */
                    emit_vu0_micro_prepare(2, 1);
                    emit_vu0_micro_multiply(3, gte_lm, 0);
                    emit_ncs_post_lighting();
#elif defined(PLATFORM_PS2)
                    /* P18 macro: preload L(VF1-4) + LC(VF7-10) once. */
                    if (gte_sf) {
                        emit_vu0_load_matrix(1, 3, 1);
                        emit_vu0_load_matrix(2, 1, 7);
                        vu0_preloaded[1] = 1;
                        vu0_preloaded[2] = 7;
                    }
                    emit_ncs_core(0, gte_sf, gte_lm);
                    emit_ncs_core(1, gte_sf, gte_lm);
                    emit_ncs_core(2, gte_sf, gte_lm);
                    vu0_preloaded[1] = 0;
                    vu0_preloaded[2] = 0;
#endif
                } else {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_NCT);
                }
                break;
            case 0x28: /* SQR */
                if (gte_use_vu0)
                {
                    /* ---- Inline SQR (fast path) ----
                     * MAC_n = IR_n² [>> 12 if sf=1]
                     * IR_n  = clamp(MAC_n, 0..0x7FFF)  [squares always ≥ 0]
                     * FLAG  = 0
                     *
                     * Scratch: T8(ir1), T9(ir2), AT(ir3), V0(sq1), V1(sq2), A0(sq3)
                     */

                    /* Load IR1,IR2,IR3 as sign-extended int16 */
                    EMIT_LH(REG_T8, CPU_CP2_DATA(9),  REG_S0);     /* T8 = (int16)IR1 */
                    EMIT_LH(REG_T9, CPU_CP2_DATA(10), REG_S0);     /* T9 = (int16)IR2 */
                    EMIT_LH(REG_AT, CPU_CP2_DATA(11), REG_S0);     /* AT = (int16)IR3 */

                    /* Square: MULT + MFLO */
                    EMIT_MULT(REG_T8, REG_T8);                     /* LO = IR1² */
                    EMIT_MFLO(REG_V0);                              /* V0 = IR1² */
                    EMIT_MULT(REG_T9, REG_T9);                     /* LO = IR2² */
                    EMIT_MFLO(REG_V1);                              /* V1 = IR2² */
                    EMIT_MULT(REG_AT, REG_AT);                     /* LO = IR3² */
                    EMIT_MFLO(REG_A0);                              /* A0 = IR3² */

                    /* sf=1: shift right by 12 */
                    if (gte_sf) {
                        EMIT_SRA(REG_V0, REG_V0, 12);              /* V0 >>= 12 */
                        EMIT_SRA(REG_V1, REG_V1, 12);              /* V1 >>= 12 */
                        EMIT_SRA(REG_A0, REG_A0, 12);              /* A0 >>= 12 */
                    }

                    /* Store MAC1-3 */
                    EMIT_SW(REG_V0, CPU_CP2_DATA(25), REG_S0);     /* MAC1 */
                    EMIT_SW(REG_V1, CPU_CP2_DATA(26), REG_S0);     /* MAC2 */
                    EMIT_SW(REG_A0, CPU_CP2_DATA(27), REG_S0);     /* MAC3 */

                    /* Saturate IR: clamp to [0..0x7FFF] — P19: PMAXW/PMINW
                     * (squares always ≥ 0, but clamp upper still needed) */
                    EMIT_ORI(REG_T8, REG_ZERO, 0x7FFF);
                    EMIT_PMINW(REG_V0, REG_V0, REG_T8);
                    EMIT_PMINW(REG_V1, REG_V1, REG_T8);
                    EMIT_PMINW(REG_A0, REG_A0, REG_T8);

                    /* Store IR1-3 + FLAG=0 */
                    EMIT_SW(REG_V0, CPU_CP2_DATA(9),  REG_S0);     /* IR1 */
                    EMIT_SW(REG_V1, CPU_CP2_DATA(10), REG_S0);     /* IR2 */
                    EMIT_SW(REG_A0, CPU_CP2_DATA(11), REG_S0);     /* IR3 */
                    EMIT_SW(REG_ZERO, CPU_CP2_CTRL(31), REG_S0);   /* FLAG = 0 */
                    /* Total: sf=0: 23 words, sf=1: 26 words */
                }
                else
                {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_SQR);
                }
                break;
            case 0x29: /* DCPL */
                if (gte_use_vu0)
                {
                    /* ---- Inline DCPL (fast path) ----
                     * acc = [R*IR1, G*IR2, B*IR3] << 4
                     * Then interpolate_color_acc + push_color
                     */

                    /* Extract R,G,B from RGBC */
                    EMIT_LW(REG_T8, CPU_CP2_DATA(6), REG_S0);      /* T8 = RGBC */
                    EMIT_ANDI(REG_V0, REG_T8, 0xFF);               /* V0 = R */
                    emit(MK_R(0, 0, REG_T8, REG_T9, 8, 0x02));    /* SRL T9,T8,8 */
                    EMIT_ANDI(REG_V1, REG_T9, 0xFF);               /* V1 = G */
                    emit(MK_R(0, 0, REG_T8, REG_T9, 16, 0x02));   /* SRL T9,T8,16 */
                    EMIT_ANDI(REG_A0, REG_T9, 0xFF);               /* A0 = B */

                    /* Load IR1-3 */
                    EMIT_LH(REG_A1, CPU_CP2_DATA(9),  REG_S0);    /* A1 = IR1 */
                    EMIT_LH(REG_A2, CPU_CP2_DATA(10), REG_S0);    /* A2 = IR2 */
                    EMIT_LH(REG_A3, CPU_CP2_DATA(11), REG_S0);    /* A3 = IR3 */

                    /* acc = (rgb × ir) << 4 */
                    EMIT_MULT(REG_V0, REG_A1);                     /* LO = R*IR1 */
                    EMIT_MFLO(REG_V0);                              /* V0 = R*IR1 */
                    EMIT_SLL(REG_V0, REG_V0, 4);                   /* V0 = acc1 */

                    EMIT_MULT(REG_V1, REG_A2);                     /* LO = G*IR2 */
                    EMIT_MFLO(REG_V1);                              /* V1 = G*IR2 */
                    EMIT_SLL(REG_V1, REG_V1, 4);                   /* V1 = acc2 */

                    EMIT_MULT(REG_A0, REG_A3);                     /* LO = B*IR3 */
                    EMIT_MFLO(REG_A0);                              /* A0 = B*IR3 */
                    EMIT_SLL(REG_A0, REG_A0, 4);                   /* A0 = acc3 */

                    emit_interpolate_color(gte_sf);
                    emit_push_color_inline();
                    emit_ir_sat_store(gte_lm);
                }
                else
                {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_DCPL);
                }
                break;
            case 0x2A: /* DPCT */
                if (gte_use_vu0)
                {
                    /* ---- Inline DPCT (fast path) ----
                     * 3× iteration of DPCS, but reads RGB0 (data[20]) instead
                     * of RGBC. push_color shifts the FIFO each iteration,
                     * so RGB0 changes between iterations.
                     * IR sat + FLAG=0 only on final iteration.
                     */
                    int dpct_iter;
                    for (dpct_iter = 0; dpct_iter < 3; dpct_iter++) {
                        /* Extract R,G,B from RGB0 and shift << 16 */
                        EMIT_LW(REG_T8, CPU_CP2_DATA(20), REG_S0);
                        EMIT_ANDI(REG_V0, REG_T8, 0xFF);
                        emit(MK_R(0, 0, REG_T8, REG_T9, 8, 0x02));
                        EMIT_ANDI(REG_V1, REG_T9, 0xFF);
                        emit(MK_R(0, 0, REG_T8, REG_T9, 16, 0x02));
                        EMIT_ANDI(REG_A0, REG_T9, 0xFF);
                        EMIT_SLL(REG_V0, REG_V0, 16);
                        EMIT_SLL(REG_V1, REG_V1, 16);
                        EMIT_SLL(REG_A0, REG_A0, 16);

                        emit_interpolate_color(gte_sf);
                        emit_push_color_inline();
                    }
                    /* IR saturation + FLAG=0 only for final result */
                    emit_ir_sat_store(gte_lm);
                }
                else
                {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_DPCT);
                }
                break;
            case 0x2D: /* AVSZ3 */
                if (gte_use_vu0)
                {
                    /* ---- Inline AVSZ3 (fast path) ----
                     * MAC0 = (int16_t)ZSF3 × (SZ1 + SZ2 + SZ3)
                     * OTZ  = saturate_sz(MAC0 >> 12)   [clamp 0..0xFFFF]
                     * FLAG = 0
                     *
                     * Scratch: T8(sum), T9(temp), AT(temp), V0(ZSF3/MAC0), V1(OTZ)
                     */

                    /* Load SZ1,SZ2,SZ3 + ZSF3 */
                    EMIT_LW(REG_T8, CPU_CP2_DATA(17), REG_S0);     /* T8 = SZ1 */
                    EMIT_LW(REG_T9, CPU_CP2_DATA(18), REG_S0);     /* T9 = SZ2 */
                    EMIT_LW(REG_AT, CPU_CP2_DATA(19), REG_S0);     /* AT = SZ3 */
                    EMIT_LH(REG_V0, CPU_CP2_CTRL(29), REG_S0);     /* V0 = (int16)ZSF3 */

                    /* Sum */
                    EMIT_ADDU(REG_T8, REG_T8, REG_T9);             /* T8 = SZ1+SZ2 */
                    EMIT_ADDU(REG_T8, REG_T8, REG_AT);             /* T8 = SZ1+SZ2+SZ3 */

                    /* Multiply + store MAC0 */
                    EMIT_MULT(REG_V0, REG_T8);                     /* HI:LO = ZSF3 × sum */
                    EMIT_MFLO(REG_V0);                              /* V0 = MAC0 */
                    EMIT_SW(REG_V0, CPU_CP2_DATA(24), REG_S0);     /* store MAC0 */

                    /* OTZ = saturate(MAC0>>12, 0, 0xFFFF) — P19: PMAXW/PMINW */
                    EMIT_SRA(REG_V1, REG_V0, 12);
                    EMIT_PMAXW(REG_V1, REG_V1, REG_ZERO);
                    EMIT_ORI(REG_T9, REG_ZERO, 0xFFFF);
                    EMIT_PMINW(REG_V1, REG_V1, REG_T9);

                    /* Store OTZ + FLAG=0 */
                    EMIT_SW(REG_V1, CPU_CP2_DATA(7), REG_S0);      /* OTZ */
                    EMIT_SW(REG_ZERO, CPU_CP2_CTRL(31), REG_S0);   /* FLAG = 0 */
                    /* Total: 19 EE words */
                }
                else
                {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_AVSZ3);
                }
                break;
            case 0x2E: /* AVSZ4 */
                if (gte_use_vu0)
                {
                    /* ---- Inline AVSZ4 (fast path) ----
                     * MAC0 = (int16_t)ZSF4 × (SZ0 + SZ1 + SZ2 + SZ3)
                     * OTZ  = saturate_sz(MAC0 >> 12)
                     * FLAG = 0
                     *
                     * Scratch: T8(sum), T9(temp), AT(temp), V0(ZSF4/MAC0), V1(OTZ)
                     */

                    /* Load SZ0-3 + ZSF4 */
                    EMIT_LW(REG_T8, CPU_CP2_DATA(16), REG_S0);     /* T8 = SZ0 */
                    EMIT_LW(REG_T9, CPU_CP2_DATA(17), REG_S0);     /* T9 = SZ1 */
                    EMIT_LW(REG_AT, CPU_CP2_DATA(18), REG_S0);     /* AT = SZ2 */
                    EMIT_LW(REG_V1, CPU_CP2_DATA(19), REG_S0);     /* V1 = SZ3 */
                    EMIT_LH(REG_V0, CPU_CP2_CTRL(30), REG_S0);     /* V0 = (int16)ZSF4 */

                    /* Sum */
                    EMIT_ADDU(REG_T8, REG_T8, REG_T9);             /* T8 = SZ0+SZ1 */
                    EMIT_ADDU(REG_T8, REG_T8, REG_AT);             /* T8 += SZ2 */
                    EMIT_ADDU(REG_T8, REG_T8, REG_V1);             /* T8 += SZ3 */

                    /* Multiply + store MAC0 */
                    EMIT_MULT(REG_V0, REG_T8);                     /* HI:LO = ZSF4 × sum */
                    EMIT_MFLO(REG_V0);                              /* V0 = MAC0 */
                    EMIT_SW(REG_V0, CPU_CP2_DATA(24), REG_S0);     /* store MAC0 */

                    /* OTZ = saturate(MAC0>>12, 0, 0xFFFF) — P19: PMAXW/PMINW */
                    EMIT_SRA(REG_V1, REG_V0, 12);
                    EMIT_PMAXW(REG_V1, REG_V1, REG_ZERO);
                    EMIT_ORI(REG_T9, REG_ZERO, 0xFFFF);
                    EMIT_PMINW(REG_V1, REG_V1, REG_T9);

                    /* Store OTZ + FLAG=0 */
                    EMIT_SW(REG_V1, CPU_CP2_DATA(7), REG_S0);      /* OTZ */
                    EMIT_SW(REG_ZERO, CPU_CP2_CTRL(31), REG_S0);   /* FLAG = 0 */
                    /* Total: 21 EE words */
                }
                else
                {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_AVSZ4);
                }
                break;
            case 0x30: /* RTPT */
                if (gte_use_vu0 && gte_sf) {
#ifdef ENABLE_VU0_MICRO
                    /* Overlapped VU0 micro: matrix once, overlap multiplies with projections */
                    emit_vu0_micro_prepare(0, 0);
                    /* V0: sync multiply (FULL — loads matrix into VF regs) */
                    emit_vu0_micro_multiply(0, gte_lm, 0);
                    /* Overlap: launch V1 on VU0 while EE projects V0 */
                    emit_vu0_micro_launch(1, 1);
                    emit_rtps_project(gte_sf, 0);
                    /* Poll V1 completion, store results */
                    emit_vu0_micro_poll_complete(gte_lm);
                    /* Overlap: launch V2 on VU0 while EE projects V1 */
                    emit_vu0_micro_launch(2, 1);
                    emit_rtps_project(gte_sf, 0);
                    /* Poll V2 completion, store results */
                    emit_vu0_micro_poll_complete(gte_lm);
                    emit_rtps_project(gte_sf, 1);
#elif defined(PLATFORM_PS2)
                    /* Macro mode: preload matrix once, reuse for all 3 */
                    emit_vu0_load_matrix(0, 0, 1);
                    vu0_preloaded[0] = 1;
                    emit_rtps_core(0, gte_sf, gte_lm, 0);
                    emit_rtps_core(1, gte_sf, gte_lm, 0);
                    emit_rtps_core(2, gte_sf, gte_lm, 1);
                    vu0_preloaded[0] = 0;
#endif
                } else {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_RTPT);
                }
                break;
            case 0x3D: /* GPF */
                if (gte_use_vu0)
                {
                    /* ---- Inline GPF (fast path) ----
                     * MAC_n = IR_n × IR0  [>> 12 if sf=1]
                     * IR_n  = clamp(MAC_n, lo..0x7FFF)
                     * push_color: RGB FIFO shift + pack MAC>>4
                     * FLAG = 0
                     *
                     * Scratch: T8(IR0), V0(MAC1), V1(MAC2), A0(MAC3)
                     *          A1-A3, T9, AT for push_color + saturation
                     */

                    /* Load IR0, IR1-3 */
                    EMIT_LH(REG_T8, CPU_CP2_DATA(8),  REG_S0);     /* T8 = IR0 */
                    EMIT_LH(REG_V0, CPU_CP2_DATA(9),  REG_S0);     /* V0 = IR1 */
                    EMIT_LH(REG_V1, CPU_CP2_DATA(10), REG_S0);     /* V1 = IR2 */
                    EMIT_LH(REG_A0, CPU_CP2_DATA(11), REG_S0);     /* A0 = IR3 */

                    /* Multiply: MAC_n = IR_n × IR0 */
                    EMIT_MULT(REG_V0, REG_T8);                     /* LO = IR1*IR0 */
                    EMIT_MFLO(REG_V0);                              /* V0 = product1 */
                    EMIT_MULT(REG_V1, REG_T8);                     /* LO = IR2*IR0 */
                    EMIT_MFLO(REG_V1);                              /* V1 = product2 */
                    EMIT_MULT(REG_A0, REG_T8);                     /* LO = IR3*IR0 */
                    EMIT_MFLO(REG_A0);                              /* A0 = product3 */

                    /* sf=1: shift for MAC */
                    if (gte_sf) {
                        EMIT_SRA(REG_V0, REG_V0, 12);
                        EMIT_SRA(REG_V1, REG_V1, 12);
                        EMIT_SRA(REG_A0, REG_A0, 12);
                    }

                    /* Store MAC1-3 */
                    EMIT_SW(REG_V0, CPU_CP2_DATA(25), REG_S0);     /* MAC1 */
                    EMIT_SW(REG_V1, CPU_CP2_DATA(26), REG_S0);     /* MAC2 */
                    EMIT_SW(REG_A0, CPU_CP2_DATA(27), REG_S0);     /* MAC3 */

                    /* ---- push_color (V0,V1,A0 = MAC values, preserved) ---- */
                    /* RGB FIFO shift */
                    EMIT_LW(REG_T8, CPU_CP2_DATA(21), REG_S0);     /* T8 = RGB1 */
                    EMIT_LW(REG_T9, CPU_CP2_DATA(22), REG_S0);     /* T9 = RGB2 */
                    EMIT_SW(REG_T8, CPU_CP2_DATA(20), REG_S0);     /* RGB0 = RGB1 */
                    EMIT_SW(REG_T9, CPU_CP2_DATA(21), REG_S0);     /* RGB1 = RGB2 */

                    /* Color: r/g/b = clamp(MAC>>4, 0, 255) — P19: PMAXW/PMINW */
                    EMIT_SRA(REG_A1, REG_V0, 4);
                    EMIT_SRA(REG_A2, REG_V1, 4);
                    EMIT_SRA(REG_A3, REG_A0, 4);

                    EMIT_ORI(REG_T9, REG_ZERO, 0xFF);
                    EMIT_PMAXW(REG_A1, REG_A1, REG_ZERO);
                    EMIT_PMINW(REG_A1, REG_A1, REG_T9);
                    EMIT_PMAXW(REG_A2, REG_A2, REG_ZERO);
                    EMIT_PMINW(REG_A2, REG_A2, REG_T9);
                    EMIT_PMAXW(REG_A3, REG_A3, REG_ZERO);
                    EMIT_PMINW(REG_A3, REG_A3, REG_T9);

                    /* Get code byte from RGBC */
                    EMIT_LW(REG_AT, CPU_CP2_DATA(6), REG_S0);      /* AT = RGBC */
                    emit(MK_R(0, 0, REG_AT, REG_AT, 24, 0x02));    /* SRL AT, AT, 24 */

                    /* Pack: RGB2 = r | (g<<8) | (b<<16) | (code<<24) */
                    EMIT_SLL(REG_AT, REG_AT, 24);                  /* AT = code<<24 */
                    EMIT_SLL(REG_A3, REG_A3, 16);                  /* A3 = b<<16 */
                    EMIT_OR(REG_AT, REG_AT, REG_A3);               /* AT |= b<<16 */
                    EMIT_SLL(REG_A2, REG_A2, 8);                   /* A2 = g<<8 */
                    EMIT_OR(REG_AT, REG_AT, REG_A2);               /* AT |= g<<8 */
                    EMIT_OR(REG_AT, REG_AT, REG_A1);               /* AT |= r */
                    EMIT_SW(REG_AT, CPU_CP2_DATA(22), REG_S0);     /* RGB2 */

                    /* ---- Saturate IR (V0,V1,A0 = MAC, still valid) — P19: PMAXW/PMINW ---- */
                    EMIT_ORI(REG_T9, REG_ZERO, 0x7FFF);
                    if (gte_lm) {
                        EMIT_PMAXW(REG_V0, REG_V0, REG_ZERO);
                        EMIT_PMINW(REG_V0, REG_V0, REG_T9);
                        EMIT_PMAXW(REG_V1, REG_V1, REG_ZERO);
                        EMIT_PMINW(REG_V1, REG_V1, REG_T9);
                        EMIT_PMAXW(REG_A0, REG_A0, REG_ZERO);
                        EMIT_PMINW(REG_A0, REG_A0, REG_T9);
                    } else {
                        EMIT_ADDIU(REG_T8, REG_ZERO, -0x8000);
                        EMIT_PMAXW(REG_V0, REG_V0, REG_T8);
                        EMIT_PMINW(REG_V0, REG_V0, REG_T9);
                        EMIT_PMAXW(REG_V1, REG_V1, REG_T8);
                        EMIT_PMINW(REG_V1, REG_V1, REG_T9);
                        EMIT_PMAXW(REG_A0, REG_A0, REG_T8);
                        EMIT_PMINW(REG_A0, REG_A0, REG_T9);
                    }

                    /* Store IR1-3 + FLAG=0 */
                    EMIT_SW(REG_V0, CPU_CP2_DATA(9),  REG_S0);     /* IR1 */
                    EMIT_SW(REG_V1, CPU_CP2_DATA(10), REG_S0);     /* IR2 */
                    EMIT_SW(REG_A0, CPU_CP2_DATA(11), REG_S0);     /* IR3 */
                    EMIT_SW(REG_ZERO, CPU_CP2_CTRL(31), REG_S0);   /* FLAG = 0 */
                }
                else
                {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_GPF);
                }
                break;
            case 0x3E: /* GPL */
                if (gte_use_vu0)
                {
                    /* ---- Inline GPL (fast path) ----
                     * Same as GPF but with MAC base:
                     * MAC_n = old_MAC_n + IR_n × IR0  [sf applied to product]
                     * If sf=1: new_MAC = old_MAC + (product >> 12)
                     * If sf=0: new_MAC = old_MAC + product
                     * Then push_color + IR saturation
                     * FLAG = 0
                     *
                     * Scratch: T8(IR0), V0(MAC1), V1(MAC2), A0(MAC3)
                     *          T9,AT for old MAC loads, A1-A3 for push_color+sat
                     */

                    /* Load IR0, IR1-3, compute products */
                    EMIT_LH(REG_T8, CPU_CP2_DATA(8),  REG_S0);     /* T8 = IR0 */
                    EMIT_LH(REG_V0, CPU_CP2_DATA(9),  REG_S0);     /* V0 = IR1 */
                    EMIT_LH(REG_V1, CPU_CP2_DATA(10), REG_S0);     /* V1 = IR2 */
                    EMIT_LH(REG_A0, CPU_CP2_DATA(11), REG_S0);     /* A0 = IR3 */

                    EMIT_MULT(REG_V0, REG_T8);                     /* LO = IR1*IR0 */
                    EMIT_MFLO(REG_V0);                              /* V0 = product1 */
                    EMIT_MULT(REG_V1, REG_T8);                     /* LO = IR2*IR0 */
                    EMIT_MFLO(REG_V1);                              /* V1 = product2 */
                    EMIT_MULT(REG_A0, REG_T8);                     /* LO = IR3*IR0 */
                    EMIT_MFLO(REG_A0);                              /* A0 = product3 */

                    /* sf=1: shift products before adding base */
                    if (gte_sf) {
                        EMIT_SRA(REG_V0, REG_V0, 12);
                        EMIT_SRA(REG_V1, REG_V1, 12);
                        EMIT_SRA(REG_A0, REG_A0, 12);
                    }

                    /* Add old MAC base */
                    EMIT_LW(REG_T8, CPU_CP2_DATA(25), REG_S0);     /* T8 = old MAC1 */
                    EMIT_LW(REG_T9, CPU_CP2_DATA(26), REG_S0);     /* T9 = old MAC2 */
                    EMIT_LW(REG_AT, CPU_CP2_DATA(27), REG_S0);     /* AT = old MAC3 */
                    EMIT_ADDU(REG_V0, REG_V0, REG_T8);             /* V0 = new MAC1 */
                    EMIT_ADDU(REG_V1, REG_V1, REG_T9);             /* V1 = new MAC2 */
                    EMIT_ADDU(REG_A0, REG_A0, REG_AT);             /* A0 = new MAC3 */

                    /* Store MAC1-3 */
                    EMIT_SW(REG_V0, CPU_CP2_DATA(25), REG_S0);     /* MAC1 */
                    EMIT_SW(REG_V1, CPU_CP2_DATA(26), REG_S0);     /* MAC2 */
                    EMIT_SW(REG_A0, CPU_CP2_DATA(27), REG_S0);     /* MAC3 */

                    /* ---- push_color (same as GPF) ---- */
                    EMIT_LW(REG_T8, CPU_CP2_DATA(21), REG_S0);     /* T8 = RGB1 */
                    EMIT_LW(REG_T9, CPU_CP2_DATA(22), REG_S0);     /* T9 = RGB2 */
                    EMIT_SW(REG_T8, CPU_CP2_DATA(20), REG_S0);     /* RGB0 = RGB1 */
                    EMIT_SW(REG_T9, CPU_CP2_DATA(21), REG_S0);     /* RGB1 = RGB2 */

                    EMIT_SRA(REG_A1, REG_V0, 4);                   /* A1 = r = MAC1>>4 */
                    EMIT_SRA(REG_A2, REG_V1, 4);                   /* A2 = g = MAC2>>4 */
                    EMIT_SRA(REG_A3, REG_A0, 4);                   /* A3 = b = MAC3>>4 */

                    emit(MK_R(0, REG_A1, REG_ZERO, REG_T8, 0, 0x2A));
                    EMIT_MOVN(REG_A1, REG_ZERO, REG_T8);
                    emit(MK_R(0, REG_A2, REG_ZERO, REG_T8, 0, 0x2A));
                    EMIT_MOVN(REG_A2, REG_ZERO, REG_T8);
                    emit(MK_R(0, REG_A3, REG_ZERO, REG_T8, 0, 0x2A));
                    EMIT_MOVN(REG_A3, REG_ZERO, REG_T8);

                    EMIT_ORI(REG_T9, REG_ZERO, 0xFF);
                    emit(MK_R(0, REG_T9, REG_A1, REG_T8, 0, 0x2A));
                    EMIT_MOVN(REG_A1, REG_T9, REG_T8);
                    emit(MK_R(0, REG_T9, REG_A2, REG_T8, 0, 0x2A));
                    EMIT_MOVN(REG_A2, REG_T9, REG_T8);
                    emit(MK_R(0, REG_T9, REG_A3, REG_T8, 0, 0x2A));
                    EMIT_MOVN(REG_A3, REG_T9, REG_T8);

                    EMIT_LW(REG_AT, CPU_CP2_DATA(6), REG_S0);
                    emit(MK_R(0, 0, REG_AT, REG_AT, 24, 0x02));    /* SRL AT,AT,24 */

                    EMIT_SLL(REG_AT, REG_AT, 24);
                    EMIT_SLL(REG_A3, REG_A3, 16);
                    EMIT_OR(REG_AT, REG_AT, REG_A3);
                    EMIT_SLL(REG_A2, REG_A2, 8);
                    EMIT_OR(REG_AT, REG_AT, REG_A2);
                    EMIT_OR(REG_AT, REG_AT, REG_A1);
                    EMIT_SW(REG_AT, CPU_CP2_DATA(22), REG_S0);     /* RGB2 */

                    /* ---- Saturate IR (same as GPF) ---- */
                    EMIT_ORI(REG_T9, REG_ZERO, 0x7FFF);
                    if (gte_lm) {
                        emit(MK_R(0, REG_V0, REG_ZERO, REG_T8, 0, 0x2A));
                        EMIT_MOVN(REG_V0, REG_ZERO, REG_T8);
                        emit(MK_R(0, REG_T9, REG_V0, REG_T8, 0, 0x2A));
                        EMIT_MOVN(REG_V0, REG_T9, REG_T8);

                        emit(MK_R(0, REG_V1, REG_ZERO, REG_T8, 0, 0x2A));
                        EMIT_MOVN(REG_V1, REG_ZERO, REG_T8);
                        emit(MK_R(0, REG_T9, REG_V1, REG_T8, 0, 0x2A));
                        EMIT_MOVN(REG_V1, REG_T9, REG_T8);

                        emit(MK_R(0, REG_A0, REG_ZERO, REG_T8, 0, 0x2A));
                        EMIT_MOVN(REG_A0, REG_ZERO, REG_T8);
                        emit(MK_R(0, REG_T9, REG_A0, REG_T8, 0, 0x2A));
                        EMIT_MOVN(REG_A0, REG_T9, REG_T8);
                    } else {
                        EMIT_ADDIU(REG_A1, REG_ZERO, -0x8000);

                        emit(MK_R(0, REG_V0, REG_A1, REG_T8, 0, 0x2A));
                        EMIT_MOVN(REG_V0, REG_A1, REG_T8);
                        emit(MK_R(0, REG_T9, REG_V0, REG_T8, 0, 0x2A));
                        EMIT_MOVN(REG_V0, REG_T9, REG_T8);

                        emit(MK_R(0, REG_V1, REG_A1, REG_T8, 0, 0x2A));
                        EMIT_MOVN(REG_V1, REG_A1, REG_T8);
                        emit(MK_R(0, REG_T9, REG_V1, REG_T8, 0, 0x2A));
                        EMIT_MOVN(REG_V1, REG_T9, REG_T8);

                        emit(MK_R(0, REG_A0, REG_A1, REG_T8, 0, 0x2A));
                        EMIT_MOVN(REG_A0, REG_A1, REG_T8);
                        emit(MK_R(0, REG_T9, REG_A0, REG_T8, 0, 0x2A));
                        EMIT_MOVN(REG_A0, REG_T9, REG_T8);
                    }

                    /* Store IR1-3 + FLAG=0 */
                    EMIT_SW(REG_V0, CPU_CP2_DATA(9),  REG_S0);     /* IR1 */
                    EMIT_SW(REG_V1, CPU_CP2_DATA(10), REG_S0);     /* IR2 */
                    EMIT_SW(REG_A0, CPU_CP2_DATA(11), REG_S0);     /* IR3 */
                    EMIT_SW(REG_ZERO, CPU_CP2_CTRL(31), REG_S0);   /* FLAG = 0 */
                }
                else
                {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_GPL);
                }
                break;
            case 0x3F: /* NCCT */
                if (gte_use_vu0 && gte_sf) {
#ifdef ENABLE_VU0_MICRO
                    /* P21: Overlapped ×3 — hide VU0 Light×V behind EE post-lighting. */
                    /* Vertex 0: full sync */
                    emit_vu0_micro_prepare(1, 3);
                    emit_vu0_micro_multiply(0, gte_lm, 0);
                    emit_vu0_micro_prepare(2, 1);
                    emit_vu0_micro_multiply(3, gte_lm, 0);
                    /* Overlap: launch Light×V1, post-light V0 */
                    emit_vu0_micro_prepare(1, 3);
                    emit_vu0_micro_launch(1, 0);
                    emit_nccs_post_lighting(gte_sf, gte_lm);
                    emit_vu0_micro_poll_complete(gte_lm);
                    /* Vertex 1: Color×IR sync */
                    emit_vu0_micro_prepare(2, 1);
                    emit_vu0_micro_multiply(3, gte_lm, 0);
                    /* Overlap: launch Light×V2, post-light V1 */
                    emit_vu0_micro_prepare(1, 3);
                    emit_vu0_micro_launch(2, 0);
                    emit_nccs_post_lighting(gte_sf, gte_lm);
                    emit_vu0_micro_poll_complete(gte_lm);
                    /* Vertex 2: Color×IR sync + final post-light */
                    emit_vu0_micro_prepare(2, 1);
                    emit_vu0_micro_multiply(3, gte_lm, 0);
                    emit_nccs_post_lighting(gte_sf, gte_lm);
#elif defined(PLATFORM_PS2)
                    /* P18 macro: preload L(VF1-4) + LC(VF7-10) once. */
                    if (gte_sf) {
                        emit_vu0_load_matrix(1, 3, 1);
                        emit_vu0_load_matrix(2, 1, 7);
                        vu0_preloaded[1] = 1;
                        vu0_preloaded[2] = 7;
                    }
                    emit_nccs_core(0, gte_sf, gte_lm);
                    emit_nccs_core(1, gte_sf, gte_lm);
                    emit_nccs_core(2, gte_sf, gte_lm);
                    vu0_preloaded[1] = 0;
                    vu0_preloaded[2] = 0;
#endif
                } else {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, gte_sf);
                    emit_load_imm32(REG_A2, gte_lm);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_Inline_NCCT);
                }
                break;
            default:
            {
                /* Unknown GTE op: fall back to generic dispatcher */
                uint32_t phys = psx_pc & 0x1FFFFFFF;
                reg_cache_invalidate();
                emit_load_imm32(REG_T8, phys);
                EMIT_ADDU(REG_T8, REG_T8, REG_S1);
                EMIT_LW(REG_A0, 0, REG_T8);
            }
                EMIT_MOVE(REG_A1, REG_S0);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Execute);
                break;
            }
        }
        break;
    }

    /* COP3 */
    case 0x13:
    {
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        reg_cache_invalidate();
        EMIT_LW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T8, REG_T8, 31, 0x02));
        uint32_t *skip_cu3 = code_ptr;
        emit(MK_I(0x05, REG_T8, REG_ZERO, 0));
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 3);
        {
            uint8_t saved_dirty = dyn_dirty_mask;
            uint32_t saved_align_cu = align_known_mask;
            emit_call_c((uint32_t)Helper_CU_Exception);
            dyn_dirty_mask = saved_dirty;
            align_known_mask = saved_align_cu;
        }
        *skip_cu3 = (*skip_cu3 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_cu3 - 1) & 0xFFFF);
        break;
    }

    /* Load instructions */
    case 0x20:
        mark_vreg_var(rt);
        emit_memory_read_signed(1, rt, rs, imm);
        break; /* LB */
    case 0x21:
        mark_vreg_var(rt);
        emit_memory_read_signed(2, rt, rs, imm);
        break; /* LH */
    case 0x23:
        mark_vreg_var(rt);
        emit_memory_read(4, rt, rs, imm, 0);
        break; /* LW */
    case 0x24:
        mark_vreg_var(rt);
        emit_memory_read(1, rt, rs, imm, 0);
        break; /* LBU */
    case 0x25:
        mark_vreg_var(rt);
        emit_memory_read(2, rt, rs, imm, 0);
        break; /* LHU */

    /* Store instructions */
    case 0x28:
        emit_memory_write(1, rt, rs, imm);
        break; /* SB */
    case 0x29:
        emit_memory_write(2, rt, rs, imm);
        break; /* SH */
    case 0x2B:
        emit_memory_write(4, rt, rs, imm);
        break; /* SW */

    /* LWL/LWR/SWL/SWR */
    case 0x22: /* LWL */
    {
        mark_vreg_var(rt);
        emit_memory_lwx(1, rt, rs, imm, dynarec_lwx_pending);
        break;
    }
    case 0x26: /* LWR */
    {
        mark_vreg_var(rt);
        emit_memory_lwx(0, rt, rs, imm, dynarec_lwx_pending);
        break;
    }
    case 0x2A: /* SWL */
    {
        emit_memory_swx(1, rt, rs, imm);
        break;
    }
    case 0x2E: /* SWR */
    {
        emit_memory_swx(0, rt, rs, imm);
        break;
    }

    /* LWC0 */
    case 0x30:
    {
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        reg_cache_invalidate();
        EMIT_LW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T8, REG_T8, 28, 0x02));
        emit(MK_I(0x0C, REG_T8, REG_T8, 1));
        uint32_t *skip_lwc0 = code_ptr;
        emit(MK_I(0x05, REG_T8, REG_ZERO, 0));
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 0);
        {
            uint8_t saved_dirty = dyn_dirty_mask;
            uint32_t saved_align_cu = align_known_mask;
            emit_call_c((uint32_t)Helper_CU_Exception);
            dyn_dirty_mask = saved_dirty;
            align_known_mask = saved_align_cu;
        }
        *skip_lwc0 = (*skip_lwc0 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_lwc0 - 1) & 0xFFFF);
        break;
    }

    /* LWC2 */
    case 0x32:
    {
        if (!block_cu2_hoisted)
        {
            flush_dirty_consts(); /* Flush before COP-usable conditional */
            reg_cache_invalidate();
            EMIT_LW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
            emit(MK_R(0, 0, REG_T8, REG_T8, 30, 0x02));
            emit(MK_I(0x0C, REG_T8, REG_T8, 1));
            uint32_t *skip_lwc2 = code_ptr;
            emit(MK_I(0x05, REG_T8, REG_ZERO, 0));
            EMIT_NOP();
            emit_load_imm32(REG_A0, psx_pc);
            emit_load_imm32(REG_A1, 2);
            {
                uint8_t saved_dirty = dyn_dirty_mask;
                uint32_t saved_align_cu = align_known_mask;
                emit_call_c((uint32_t)Helper_CU_Exception);
                dyn_dirty_mask = saved_dirty;
                align_known_mask = saved_align_cu;
            }
            *skip_lwc2 = (*skip_lwc2 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_lwc2 - 1) & 0xFFFF);
        }

        /* Memory read via LUT fast path (result in V0), then GTE write */
        {
            int saved_defer = dynarec_load_defer;
            dynarec_load_defer = 1;
            emit_memory_read(4, 0, rs, imm, 0); /* V0 = word from [rs+imm] */
            dynarec_load_defer = saved_defer;
        }
        /* Inline GTE write for simple data registers (same as MTC2 P3) */
        {
            int rt5 = rt & 0x1F;
            switch (rt5)
            {
            case 15:
            case 28:
            case 30:
                /* Complex: SXY FIFO / IRGB / LZCS — keep C call (lite) */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, rt5);
                EMIT_MOVE(6, REG_V0);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_WriteData);
                break;
            case 29:
            case 31:
                /* Read-only: no-op (just discard loaded value) */
                break;
            case 1:
            case 3:
            case 5:
            case 8:
            case 9:
            case 10:
            case 11:
                /* Sign-extend 16 + store */
                emit(MK_R(0, 0, REG_V0, REG_V0, 16, 0x00)); /* sll v0, 16 */
                emit(MK_R(0, 0, REG_V0, REG_V0, 16, 0x03)); /* sra v0, 16 */
                EMIT_SW(REG_V0, CPU_CP2_DATA(rt5), REG_S0);
                break;
            case 7:
            case 16:
            case 17:
            case 18:
            case 19:
                /* Zero-extend 16 + store */
                emit(MK_I(0x0C, REG_V0, REG_V0, 0xFFFF));
                EMIT_SW(REG_V0, CPU_CP2_DATA(rt5), REG_S0);
                break;
            default:
                /* Simple store */
                EMIT_SW(REG_V0, CPU_CP2_DATA(rt5), REG_S0);
                break;
            }
        }
    }
    break;

    /* LWC3 */
    case 0x33:
    {
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        reg_cache_invalidate();
        EMIT_LW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T8, REG_T8, 31, 0x02));
        uint32_t *skip_lwc3 = code_ptr;
        emit(MK_I(0x05, REG_T8, REG_ZERO, 0));
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 3);
        {
            uint8_t saved_dirty = dyn_dirty_mask;
            uint32_t saved_align_cu = align_known_mask;
            emit_call_c((uint32_t)Helper_CU_Exception);
            dyn_dirty_mask = saved_dirty;
            align_known_mask = saved_align_cu;
        }
        *skip_lwc3 = (*skip_lwc3 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_lwc3 - 1) & 0xFFFF);
        break;
    }

    /* SWC0 */
    case 0x38:
    {
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        reg_cache_invalidate();
        EMIT_LW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T8, REG_T8, 28, 0x02));
        emit(MK_I(0x0C, REG_T8, REG_T8, 1));
        uint32_t *skip_swc0 = code_ptr;
        emit(MK_I(0x05, REG_T8, REG_ZERO, 0));
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 0);
        {
            uint8_t saved_dirty = dyn_dirty_mask;
            uint32_t saved_align_cu = align_known_mask;
            emit_call_c((uint32_t)Helper_CU_Exception);
            dyn_dirty_mask = saved_dirty;
            align_known_mask = saved_align_cu;
        }
        *skip_swc0 = (*skip_swc0 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_swc0 - 1) & 0xFFFF);
        break;
    }

    /* SWC2 */
    case 0x3A:
    {
        if (!block_cu2_hoisted)
        {
            flush_dirty_consts(); /* Flush before COP-usable conditional */
            reg_cache_invalidate();
            EMIT_LW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
            emit(MK_R(0, 0, REG_T8, REG_T8, 30, 0x02));
            emit(MK_I(0x0C, REG_T8, REG_T8, 1));
            uint32_t *skip_swc2 = code_ptr;
            emit(MK_I(0x05, REG_T8, REG_ZERO, 0));
            EMIT_NOP();
            emit_load_imm32(REG_A0, psx_pc);
            emit_load_imm32(REG_A1, 2);
            {
                uint8_t saved_dirty = dyn_dirty_mask;
                uint32_t saved_align_cu = align_known_mask;
                emit_call_c((uint32_t)Helper_CU_Exception);
                dyn_dirty_mask = saved_dirty;
                align_known_mask = saved_align_cu;
            }
            *skip_swc2 = (*skip_swc2 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_swc2 - 1) & 0xFFFF);
        }

        /* Inline GTE read for simple data registers (same as MFC2 P3) */
        {
            int rt5 = rt & 0x1F;
            switch (rt5)
            {
            case 28:
            case 29:
                /* Complex: IRGB/ORGB clamp — keep C call (lite) */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, rt5);
                emit_call_c_lite((uint32_t)GTE_ReadData);
                break;
            case 15:
                /* SXY2 always returns D(14) */
                EMIT_LW(REG_V0, CPU_CP2_DATA(14), REG_S0);
                break;
            default:
                /* Simple read */
                EMIT_LW(REG_V0, CPU_CP2_DATA(rt5), REG_S0);
                break;
            }
        }

        /* Memory write: data in V0, addr from base+offset.
         * Use cold_queue for slow path (P7: deferred to block end). */
        EMIT_MOVE(REG_T9, REG_V0); /* T9 = data (cold path protocol) */
        emit_load_psx_reg(REG_T8, rs);
        EMIT_ADDIU(REG_T8, REG_T8, imm); /* T8 = effective addr */

        /* Flush lazy consts before conditional fast/slow split */
        flush_dirty_consts();

        /* P22: Inline ISC skip for SWC2 when SMRV+aligned (same as SW) */
        if (block_isc_cached
            && smrv_is_known_ram(rs)
            && align_is_known(rs) && (imm % 4 == 0))
        {
            EMIT_LW(REG_AT, 80, REG_SP);                     /* at = cached ISC     */
            uint32_t *isc_skip = code_ptr;
            emit(MK_I(0x05, REG_AT, REG_ZERO, 0));           /* bne at,zero,@skip   */
            emit(MK_R(0, REG_T8, REG_S3, REG_AT, 0, 0x24)); /* [delay] and at,t8,s3 */
            EMIT_ADDU(REG_AT, REG_AT, REG_S1);               /* addu at, at, s1     */
            EMIT_SW(REG_T9, 0, REG_AT);                      /* store               */
            int32_t skip_off = (int32_t)(code_ptr - isc_skip - 1);
            *isc_skip = (*isc_skip & 0xFFFF0000) | ((uint32_t)skip_off & 0xFFFF);
        }
        else
        {
        /* Cache Isolation check */
        uint32_t *isc_swc2;
        if (block_isc_cached)
        {
            EMIT_LW(REG_AT, 80, REG_SP);
            isc_swc2 = code_ptr;
            emit(MK_I(0x05, REG_AT, REG_ZERO, 0)); /* bne at,zero,@cold */
            EMIT_NOP();
        }
        else
        {
            EMIT_LW(REG_A0, CPU_COP0(12), REG_S0);
            emit(MK_R(0, 0, REG_A0, REG_A0, 16, 0x02));
            emit(MK_I(0x0C, REG_A0, REG_A0, 1));
            isc_swc2 = code_ptr;
            emit(MK_I(0x05, REG_A0, REG_ZERO, 0)); /* bne → cold */
            EMIT_NOP();
        }

        /* Alignment check (word-aligned required) — P6: elide if base known aligned */
        uint32_t *align_swc2 = NULL;
        if (align_is_known(rs) && (imm % 4 == 0))
        {
            /* Alignment guaranteed — emit only the phys mask */
            emit(MK_R(0, REG_T8, REG_S3, REG_AT, 0, 0x24)); /* and at, t8, s3 */
        }
        else
        {
            emit(MK_I(0x0C, REG_T8, REG_AT, 3)); /* andi at, t8, 3 */
            align_swc2 = code_ptr;
            emit(MK_I(0x05, REG_AT, REG_ZERO, 0));          /* bne at, zero, @cold */
            emit(MK_R(0, REG_T8, REG_S3, REG_AT, 0, 0x24)); /* [delay] and at, t8, s3 */
        }

        /* Range check: skip if SMRV proves base is RAM */
        uint32_t *range_swc2 = NULL;
        if (!smrv_is_known_ram(rs))
        {
            emit(MK_R(0, 0, REG_AT, REG_A0, 21, 0x02)); /* srl a0, at, 21 */
            range_swc2 = code_ptr;
            emit(MK_I(0x05, REG_A0, REG_ZERO, 0)); /* bne a0, zero, @cold */
        }
        EMIT_ADDU(REG_AT, REG_AT, REG_S1); /* [delay/inline] host addr */

        /* Fast path: direct store */
        EMIT_SW(REG_T9, 0, REG_AT);

        /* Defer slow path to end of block via cold_queue (P7) */
        {
            uint32_t *branches[4];
            int nb = 0;
            branches[nb++] = isc_swc2;
            if (align_swc2)
                branches[nb++] = align_swc2;
            if (range_swc2)
                branches[nb++] = range_swc2;
            cold_slow_push(branches, nb, code_ptr,
                           (uint32_t)WriteWord, psx_pc,
                           (int16_t)emit_cycle_offset, 4, 1, 1,
                           dyn_dirty_mask);
        }
        }
        reg_cache_invalidate();
    }
    break;

    /* SWC3 */
    case 0x3B:
    {
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        reg_cache_invalidate();
        EMIT_LW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T8, REG_T8, 31, 0x02));
        uint32_t *skip_swc3 = code_ptr;
        emit(MK_I(0x05, REG_T8, REG_ZERO, 0));
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 3);
        {
            uint8_t saved_dirty = dyn_dirty_mask;
            uint32_t saved_align_cu = align_known_mask;
            emit_call_c((uint32_t)Helper_CU_Exception);
            dyn_dirty_mask = saved_dirty;
            align_known_mask = saved_align_cu;
        }
        *skip_swc3 = (*skip_swc3 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_swc3 - 1) & 0xFFFF);
        break;
    }

    default:
    {
        static int unknown_log_count = 0;
        if (unknown_log_count < 200)
        {
            DLOG("Unknown opcode 0x%02" PRIX32 " at PC=0x%08" PRIX32 "\n", op, psx_pc);
            unknown_log_count++;
        }
        break;
    }
    }
    return 0;
}
