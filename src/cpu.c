#include <tamtypes.h>
#include <stdio.h>
#include <kernel.h>
#include "superpsx.h"

R3000CPU cpu;

/* ---- PSX Exception Handling ---- */
void PSX_Exception(u32 cause_code) {
    /* Save EPC and set Cause */
    cpu.cop0[COP0_EPC] = cpu.pc;
    cpu.cop0[COP0_CAUSE] = (cpu.cop0[COP0_CAUSE] & ~0x7C) | ((cause_code & 0x1F) << 2);

    /* Push exception mode: shift Status bits left by 2 */
    u32 sr = cpu.cop0[COP0_SR];
    u32 mode_bits = sr & 0x3F;
    sr = (sr & ~0x3F) | ((mode_bits << 2) & 0x3F);
    sr |= 0x02; /* Set EXL bit (KUc=0, IEc=0) */
    cpu.cop0[COP0_SR] = sr;

    /* Jump to exception vector */
    if (sr & 0x00400000) {
        /* BEV=1: vector in BIOS */
        cpu.pc = 0xBFC00180;
    } else {
        /* BEV=0: vector in RAM */
        cpu.pc = 0x80000080;
    }
}

/* ---- PSX Syscall Handler ---- */
/*
 * PSX BIOS syscalls use register $t1 ($9) or function number
 * convention: SYSCALL with the function number in a specific register.
 * The BIOS uses three tables: A(0xA0), B(0xB0), C(0xC0)
 * with the function number in $t1.
 */
void Handle_Syscall(void) {
    u32 pc = cpu.pc;
    u32 code = (cpu.regs[4] >> 6) & 0xFFFFF; /* Extract code from $a0 if available */

    /* Let the PSX BIOS handle it via exception vector */
    PSX_Exception(0x08); /* Syscall exception code */
}

/* ---- EE Exception Handler (for catching native faults) ---- */
static void EE_ExceptionHandler(int cause) {
    u32 epc;
    asm volatile("mfc0 %0, $14" : "=r"(epc));
    u32 badvaddr;
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
    while (1);
}

/* ---- Init ---- */
void Init_CPU(void) {
    printf("Initializing CPU...\n");

    /* Clear CPU state */
    int i;
    for (i = 0; i < 32; i++) {
        cpu.regs[i] = 0;
        cpu.cop0[i] = 0;
    }
    cpu.pc = 0;
    cpu.hi = 0;
    cpu.lo = 0;

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
