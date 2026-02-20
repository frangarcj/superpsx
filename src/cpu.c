#include <stdint.h>
#include <stdio.h>
#include <kernel.h>
#include "superpsx.h"

#define LOG_TAG "EXC"

R3000CPU cpu;

/* Exception support for dynarec mid-block exceptions */
jmp_buf psx_block_jmp;
volatile int psx_block_exception = 0;
uint32_t psx_abort_pc = 0;            /* saved exception-handler PC            */

/* ---- PSX Exception Handling ---- */
static int cdrom_irq_count = 0;
void PSX_Exception(uint32_t cause_code)
{
    /* Debug: log CD-ROM interrupt delivery (first 20) */
    if (cause_code == 0 && cdrom_irq_count < 20)
    {
        uint32_t istat = CheckInterrupts();
        if (istat & 0x04)
        {
            DLOG("Delivering CD-ROM interrupt #%d! PC=%08X SR=%08X\n",
                 cdrom_irq_count, (unsigned)cpu.pc, (unsigned)cpu.cop0[PSX_COP0_SR]);
            cdrom_irq_count++;
        }
    }

    /* Save EPC and set Cause */
    cpu.cop0[PSX_COP0_EPC] = cpu.pc;

    /* Set ExcCode in Cause register bits [6:2].
     * Clear BD bit (bit 31) since we don't currently track branch delay
     * slot exceptions. Preserve only IP bits [15:8]. */
    uint32_t cause = cpu.cop0[PSX_COP0_CAUSE] & 0x0000FF00; /* Keep only IP bits */
    cause |= ((cause_code & 0x1F) << 2);

    /* For hardware interrupts (cause_code == 0), set IP2 (bit 10)
     * to indicate a pending hardware interrupt on the R3000A's
     * single external interrupt line. The BIOS checks this bit. */
    if (cause_code == 0)
    {
        cause |= (1 << 10); /* IP2 = hardware interrupt pending */
    }
    cpu.cop0[PSX_COP0_CAUSE] = cause;

    /* Push exception mode: shift Status bits [5:0] left by 2
     * This pushes IEc→IEp→IEo, KUc→KUp→KUo
     * New IEc=0, KUc=0 (kernel mode, interrupts disabled) */
    uint32_t sr = cpu.cop0[PSX_COP0_SR];
    uint32_t mode_bits = sr & 0x3F;
    sr = (sr & ~0x3F) | ((mode_bits << 2) & 0x3F);
    /* IEc=0, KUc=0 already from the shift (bits 0,1 are 0) */
    cpu.cop0[PSX_COP0_SR] = sr;

    /* Jump to exception vector */
    if (sr & 0x00400000)
    {
        /* BEV=1: vector in BIOS */
        cpu.pc = 0xBFC00180;
    }
    else
    {
        /* BEV=0: vector in RAM */
        /* Verify exception handler is installed */
        uint32_t handler_word = *(uint32_t *)(psx_ram + 0x80);
        if (handler_word == 0 && cause_code == 0)
        {
            static int exc_warn = 0;
            if (exc_warn < 5)
            {
                DLOG("WARNING: No exception handler at 0x80000080! (word=0x%08X) Ignoring IRQ.\n",
                     (unsigned)handler_word);
                exc_warn++;
            }
            /* Undo the mode push since we can't handle it */
            cpu.cop0[PSX_COP0_SR] = sr;      /* Restore original SR */
            cpu.pc = cpu.cop0[PSX_COP0_EPC]; /* Restore PC */
            return;
        }
        cpu.pc = 0x80000080;
    }

    /* If we're inside a dynarec block, signal early abort instead of longjmp */
    if (psx_block_exception)
    {
        cpu.block_aborted = 1;
        psx_abort_pc = cpu.pc;
        return;
    }
}

/* ---- Exception helpers for dynarec ---- */
/* SYSCALL: always triggers exception code 8 */
void Helper_Syscall_Exception(uint32_t pc)
{
    cpu.pc = pc;
    PSX_Exception(0x08);
}

/* BREAK: always triggers exception code 9 */
void Helper_Break_Exception(uint32_t pc)
{
    cpu.pc = pc;
    PSX_Exception(0x09);
}

/* Coprocessor Unusable: exception code 11, with CE field in Cause bits 28-29 */
void Helper_CU_Exception(uint32_t pc, uint32_t cop_num)
{
    cpu.pc = pc;
    /* Set CE field (bits 28-29) in Cause BEFORE calling PSX_Exception.
     * PSX_Exception preserves IP bits [15:8] and sets ExcCode [6:2].
     * We need to also set CE, so we pre-set it in Cause. */
    uint32_t cause = cpu.cop0[PSX_COP0_CAUSE] & 0x0000FF00; /* Keep IP bits */
    cause |= ((0x0B & 0x1F) << 2);                          /* ExcCode = 11 (CpU) */
    cause |= ((cop_num & 0x3) << 28);                       /* CE field */
    cpu.cop0[PSX_COP0_CAUSE] = cause;

    /* Push exception mode stack (same as PSX_Exception) */
    uint32_t sr = cpu.cop0[PSX_COP0_SR];
    uint32_t mode_bits = sr & 0x3F;
    sr = (sr & ~0x3F) | ((mode_bits << 2) & 0x3F);
    cpu.cop0[PSX_COP0_SR] = sr;

    /* Save EPC */
    cpu.cop0[PSX_COP0_EPC] = pc;

    /* Jump to exception vector */
    if (sr & 0x00400000)
        cpu.pc = 0xBFC00180;
    else
        cpu.pc = 0x80000080;

    if (psx_block_exception)
    {
        cpu.block_aborted = 1;
        psx_abort_pc = cpu.pc;
        return;
    }
}

/* ADD with overflow detection: if overflow, trigger exception 0x0C */
void Helper_ADD(uint32_t rs_val, uint32_t rt_val, uint32_t rd, uint32_t pc)
{
    uint32_t result = rs_val + rt_val;
    /* Overflow if: operands have same sign but result has different sign */
    if (!((rs_val ^ rt_val) & 0x80000000) && ((result ^ rs_val) & 0x80000000))
    {
        cpu.pc = pc;
        PSX_Exception(0x0C); /* Overflow */
        return;              /* Won't reach here - longjmp fires */
    }
    if (rd != 0)
        cpu.regs[rd] = result;
}

/* SUB with overflow detection */
void Helper_SUB(uint32_t rs_val, uint32_t rt_val, uint32_t rd, uint32_t pc)
{
    uint32_t result = rs_val - rt_val;
    /* Overflow if: operands have different signs and result sign != rs sign */
    if (((rs_val ^ rt_val) & 0x80000000) && ((result ^ rs_val) & 0x80000000))
    {
        cpu.pc = pc;
        PSX_Exception(0x0C); /* Overflow */
        return;
    }
    if (rd != 0)
        cpu.regs[rd] = result;
}

/* ADDI with overflow detection */
void Helper_ADDI(uint32_t rs_val, uint32_t imm_sext, uint32_t rt, uint32_t pc)
{
    uint32_t result = rs_val + imm_sext;
    if (!((rs_val ^ imm_sext) & 0x80000000) && ((result ^ rs_val) & 0x80000000))
    {
        cpu.pc = pc;
        PSX_Exception(0x0C); /* Overflow */
        return;
    }
    if (rt != 0)
        cpu.regs[rt] = result;
}

/* ---- PSX Syscall Handler ---- */
/*
 * PSX BIOS syscalls use register $t1 ($9) or function number
 * convention: SYSCALL with the function number in a specific register.
 * The BIOS uses three tables: A(0xA0), B(0xB0), C(0xC0)
 * with the function number in $t1.
 */
void Handle_Syscall(void)
{
    /* PSX BIOS syscalls:
     * The function number is in $a0 (register 4).
     * Syscall 0 = NoFunction
     * Syscall 1 = EnterCriticalSection (returns old IEc, disables interrupts)
     * Syscall 2 = ExitCriticalSection (enables interrupts)
     * Syscall 3 = ChangeThreadSubFunction
     * Others are handled by the BIOS exception handler.
     */
    uint32_t func = cpu.regs[4]; /* $a0 = function number */
    static int syscall_log_count = 0;

    if (syscall_log_count < 50)
    {
        //        DLOG("[SYSCALL] func=%u PC=%08X SR=%08X\n",
        //               (unsigned)func, (unsigned)cpu.pc,
        //               (unsigned)cpu.cop0[PSX_COP0_SR]);
        syscall_log_count++;
    }

    switch (func)
    {
    case 0: /* NoFunction */
        /* Just return, advance PC past SYSCALL */
        cpu.pc += 4; /* Skip SYSCALL instruction */
        return;

    case 1: /* EnterCriticalSection */
    {
        uint32_t sr = cpu.cop0[PSX_COP0_SR];
        cpu.regs[2] = sr & 1;            /* $v0 = old IEc bit */
        cpu.cop0[PSX_COP0_SR] = sr & ~1; /* Clear IEc */
        cpu.pc += 4;
        if (syscall_log_count < 100)
        {
            //                DLOG("EnterCriticalSection: SR %08X -> %08X, old_IEc=%d\n",
            //                       (unsigned)sr, (unsigned)cpu.cop0[PSX_COP0_SR],
            //                       (int)cpu.regs[2]);
        }
    }
        return;

    case 2: /* ExitCriticalSection */
    {
        uint32_t sr = cpu.cop0[PSX_COP0_SR];
        sr |= 0x00000401; /* IEc=1 (bit 0) + IM2=1 (bit 10) for HW interrupts */
        cpu.cop0[PSX_COP0_SR] = sr;
        cpu.pc += 4;
        if (syscall_log_count < 100)
        {
            //                DLOG("ExitCriticalSection: SR -> %08X (IEc=%d IM2=%d)\n",
            //                       (unsigned)sr, (int)(sr & 1), (int)((sr >> 10) & 1));
        }
    }
        return;

    case 3: /* ChangeThreadSubFunction */
        cpu.pc += 4;
        return;

    default:
        /* Unknown syscall - delegate to BIOS exception handler */
        PSX_Exception(0x08); /* Syscall exception code */
        return;
    }
}

/* ---- EE Exception Handler (for catching native faults) ---- */
static void EE_ExceptionHandler(int cause)
{
    uint32_t epc;
    asm volatile("mfc0 %0, $14" : "=r"(epc));
    uint32_t badvaddr;
    asm volatile("mfc0 %0, $8" : "=r"(badvaddr));

    printf("EE EXCEPTION: cause=%d EPC=0x%08X BadVAddr=0x%08X\n",
           cause, (unsigned)epc, (unsigned)badvaddr);
    printf("  PSX PC=0x%08X\n", (unsigned)cpu.pc);
    printf("  PSX regs: at=%08X v0=%08X v1=%08X a0=%08X\n",
           (unsigned)cpu.regs[1], (unsigned)cpu.regs[2],
           (unsigned)cpu.regs[3], (unsigned)cpu.regs[4]);
    printf("  a1=%08X a2=%08X a3=%08X t0=%08X\n",
           (unsigned)cpu.regs[5], (unsigned)cpu.regs[6],
           (unsigned)cpu.regs[7], (unsigned)cpu.regs[8]);
    printf("  sp=%08X ra=%08X\n",
           (unsigned)cpu.regs[29], (unsigned)cpu.regs[31]);

    printf("Halting.\n");
    SleepThread();
}

/* ---- Init ---- */
void Init_CPU(void)
{
    printf("Initializing CPU...\n");

    /* Clear CPU state */
    int i;
    for (i = 0; i < 32; i++)
    {
        cpu.regs[i] = 0;
        cpu.cop0[i] = 0;
    }
    cpu.pc = 0;
    cpu.hi = 0;
    cpu.lo = 0;
    cpu.i_stat = 0;
    cpu.i_mask = 0;

    /* Install EE exception handlers for debug */
    /* DISABLED: SetVCommonHandler may be corrupting stdout/kernel state */
    /*
    int causes[] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    for (i = 0; i < 10; i++) {
        SetVCommonHandler(causes[i], (void *)EE_ExceptionHandler);
    }
    */

    printf("CPU initialized.\n");
    fflush(stdout);
}
