/*
 * tlb_handler.c - PS2 TLB fast-path for PSX emulated memory
 *
 * Maps the PSX memory space using hardware TLB entries:
 *   - psx_ram      at VA 0x20000000 (1MB pages, 2MB, 1 TLB entry)
 *   - scratchpad   at VA 0x3F800000 (4KB page, 1KB used, 1 TLB entry)
 *   - psx_bios     at VA 0x3FC00000 (256KB pages, 512KB, 1 TLB entry)
 *   - HW I/O at VA 0x3F801000+: INVALID page → TLB miss → exception → C helpers
 *
 * JIT memory accesses use VA = (psx_addr & 0x1FFFFFFF) + 0x20000000.
 * RAM/scratchpad/BIOS accesses hit the TLB → zero-overhead direct access.
 * Hardware I/O accesses miss → exception handler → trampoline → ReadWord/WriteWord.
 */

#include <kernel.h>
#include <string.h>
#include "superpsx.h"

/* VA base for TLB-mapped PSX RAM */
#define PSX_TLB_BASE       0x20000000
#define PSX_TLB_SIZE       PSX_RAM_SIZE /* 2 MB */

/* EE Scratchpad communication area (last 32 bytes of 16KB SP) */
#define SP_BASE             0x70000000
#define SP_FAULT_EPC        0x3FE0      /* offset: saved EPC */
#define SP_FAULT_INSN       0x3FE4      /* offset: saved instruction */
#define SP_TRAMPOLINE_ADDR  0x3FE8      /* offset: trampoline VA */
#define SP_ORIG_HANDLER     0x3FEC      /* offset: original handler copy */

/* Exported: the TLB-mapped base address for JIT S1 register */
uint32_t psx_tlb_base = 0;

/* Buffer for the original TLB refill handler code (copied from 0x80000000) */
static uint32_t orig_handler_copy[32] __attribute__((aligned(64)));

/* Memory helpers declared in superpsx.h */

/* ================================================================
 *  TLB trampoline: jumped to via ERET from the exception handler.
 *  All JIT registers are intact:
 *    T0 ($8)  = PSX address
 *    T2 ($10) = write value (for stores)
 *    V0 ($2)  = load destination
 *
 *  Scratchpad has:
 *    [SP_FAULT_EPC]  = EPC of faulting instruction
 *    [SP_FAULT_INSN] = faulting instruction word
 *
 *  The trampoline saves caller-saved registers, calls the appropriate
 *  C helper, restores registers (with V0 = read result for loads),
 *  and jumps to EPC+4 (the instruction after the fault).
 * ================================================================ */
asm(
    ".section .text\n"
    ".align 6\n"
    ".globl TLB_Trampoline\n"
    ".type TLB_Trampoline, @function\n"
    "TLB_Trampoline:\n"
    ".set push\n"
    ".set noreorder\n"
    ".set noat\n"

    /* Save caller-saved registers */
    "addiu $sp, $sp, -80\n"
    "sw $ra,  0($sp)\n"
    "sw $4,   4($sp)\n"    /* a0 */
    "sw $5,   8($sp)\n"    /* a1 */
    "sw $2,  12($sp)\n"    /* v0 */
    "sw $3,  16($sp)\n"    /* v1 */
    "sw $8,  20($sp)\n"    /* t0 */
    "sw $9,  24($sp)\n"    /* t1 */
    "sw $10, 28($sp)\n"    /* t2 */
    "sw $11, 32($sp)\n"    /* t3 */
    "sw $12, 36($sp)\n"    /* t4 */
    "sw $13, 40($sp)\n"    /* t5 */
    "sw $14, 44($sp)\n"    /* t6 */
    "sw $15, 48($sp)\n"    /* t7 */
    "sw $24, 52($sp)\n"    /* t8 */
    "sw $25, 56($sp)\n"    /* t9 */
    "sw $1,  60($sp)\n"    /* at */
    "sw $28, 64($sp)\n"    /* gp */

    /* Read faulting instruction opcode from scratchpad */
    "lui  $9, 0x7000\n"
    "lw   $11, 0x3FE4($9)\n"    /* t3 = insn word */
    "srl  $12, $11, 26\n"       /* t4 = opcode */

    /* a0 = PSX address (from saved $8 / t0) */
    "lw   $4, 20($sp)\n"

    /* Store or Load? (opcode >= 0x28 -> store) */
    "sltiu $1, $12, 0x28\n"
    "beqz  $1, .Ltlb_write\n"
    "nop\n"

    /* ---- READ PATH ---- */
    "addiu $1, $12, -0x23\n"   /* LW? */
    "beqz  $1, .Ltlb_lw\n"
    "nop\n"
    "addiu $1, $12, -0x25\n"   /* LHU? */
    "beqz  $1, .Ltlb_lhu\n"
    "nop\n"
    "addiu $1, $12, -0x24\n"   /* LBU? */
    "beqz  $1, .Ltlb_lbu\n"
    "nop\n"
    "addiu $1, $12, -0x21\n"   /* LH? */
    "beqz  $1, .Ltlb_lh\n"
    "nop\n"
    "addiu $1, $12, -0x20\n"   /* LB? */
    "beqz  $1, .Ltlb_lb\n"
    "nop\n"
    /* Default: treat as LW */
    "j .Ltlb_lw\n"
    "nop\n"

    ".Ltlb_lw:\n"
    "jal ReadWord\n"
    "nop\n"
    "j .Ltlb_read_done\n"
    "nop\n"

    ".Ltlb_lhu:\n"
    "jal ReadHalf\n"
    "nop\n"
    /* zero-extend: ReadHalf returns uint16_t, already correct */
    "j .Ltlb_read_done\n"
    "nop\n"

    ".Ltlb_lbu:\n"
    "jal ReadByte\n"
    "nop\n"
    "j .Ltlb_read_done\n"
    "nop\n"

    ".Ltlb_lh:\n"
    "jal ReadHalf\n"
    "nop\n"
    "sll $2, $2, 16\n"
    "sra $2, $2, 16\n"
    "j .Ltlb_read_done\n"
    "nop\n"

    ".Ltlb_lb:\n"
    "jal ReadByte\n"
    "nop\n"
    "sll $2, $2, 24\n"
    "sra $2, $2, 24\n"
    "j .Ltlb_read_done\n"
    "nop\n"

    ".Ltlb_read_done:\n"
    "sw $2, 12($sp)\n"
    "j .Ltlb_exit\n"
    "nop\n"

    /* ---- WRITE PATH ---- */
    ".Ltlb_write:\n"
    "lw $5, 28($sp)\n"         /* a1 = saved t2 = write value */

    "addiu $1, $12, -0x2B\n"   /* SW? */
    "beqz  $1, .Ltlb_sw\n"
    "nop\n"
    "addiu $1, $12, -0x29\n"   /* SH? */
    "beqz  $1, .Ltlb_sh\n"
    "nop\n"
    "addiu $1, $12, -0x28\n"   /* SB? */
    "beqz  $1, .Ltlb_sb\n"
    "nop\n"
    /* Default: treat as SW */
    "j .Ltlb_sw\n"
    "nop\n"

    ".Ltlb_sw:\n"
    "jal WriteWord\n"
    "nop\n"
    "j .Ltlb_exit\n"
    "nop\n"

    ".Ltlb_sh:\n"
    "jal WriteHalf\n"
    "nop\n"
    "j .Ltlb_exit\n"
    "nop\n"

    ".Ltlb_sb:\n"
    "jal WriteByte\n"
    "nop\n"

    /* ---- COMMON EXIT ---- */
    ".Ltlb_exit:\n"
    /* Backpatch: patch faulting JIT code so this access never misses again */
    "lui  $9, 0x7000\n"
    "lw   $4, 0x3FE0($9)\n"       /* a0 = saved EPC (fault_insn_addr) */
    "jal  TLB_Backpatch\n"
    "nop\n"

    /* Compute return address = saved EPC + 4 */
    "lui  $9, 0x7000\n"
    "lw   $9, 0x3FE0($9)\n"    /* t1 = saved EPC */
    "addiu $9, $9, 4\n"        /* next insn */
    "sw   $9, 24($sp)\n"       /* overwrite saved t1 */

    /* Restore registers */
    "lw $ra,  0($sp)\n"
    "lw $4,   4($sp)\n"
    "lw $5,   8($sp)\n"
    "lw $2,  12($sp)\n"        /* v0 = read result */
    "lw $3,  16($sp)\n"
    "lw $8,  20($sp)\n"
    "lw $9,  24($sp)\n"        /* t1 = return address */
    "lw $10, 28($sp)\n"
    "lw $11, 32($sp)\n"
    "lw $12, 36($sp)\n"
    "lw $13, 40($sp)\n"
    "lw $14, 44($sp)\n"
    "lw $15, 48($sp)\n"
    "lw $24, 52($sp)\n"
    "lw $25, 56($sp)\n"
    "lw $1,  60($sp)\n"
    "lw $28, 64($sp)\n"
    "addiu $sp, $sp, 80\n"

    /* Jump to instruction after fault (t1 = $9) */
    "jr $9\n"
    "nop\n"

    ".set pop\n"
);

/* Declare the global asm trampoline symbol */
extern void TLB_Trampoline(void);

/* ================================================================
 *  Install TLB refill handler stub at exception vector 0x80000000
 *
 *  The stub checks BadVAddr; if it's in our TLB range, redirects to
 *  the trampoline via ERET.  Otherwise, chains to the original handler.
 * ================================================================ */
static void install_tlb_handler(void)
{
    uint32_t *sp = (uint32_t *)SP_BASE;

    /* Save original handler (32 instructions = 128 bytes) */
    memcpy(orig_handler_copy, (void *)0x80000000, 128);
    FlushCache(0); /* writeback D-cache for the copy */

    /* Store addresses in scratchpad for the handler stub */
    sp[SP_TRAMPOLINE_ADDR / 4] = (uint32_t)TLB_Trampoline;
    sp[SP_ORIG_HANDLER / 4]    = (uint32_t)orig_handler_copy;

    /* Build handler stub (raw MIPS instructions at 0x80000000)
     *
     * The stub must be <= 32 instructions (128 bytes) before
     * the next exception vector at 0x80000080.
     *
     * Register usage: k0 ($26), k1 ($27) only.
     */
    uint32_t stub[32];
    int n = 0;

    /* mfc0 k1, BadVAddr ($8) */
    stub[n++] = 0x40188000;  /* mfc0 $27, $8 */

    /* lui k0, 0x4000 (upper bound = 0x40000000, covers entire PSX VA range) */
    stub[n++] = 0x3C1A4000;  /* lui $26, 0x4000 */

    /* sltu k0, k1, k0  (k0 = (BadVAddr < 0x20200000) ? 1 : 0) */
    stub[n++] = 0x037AD02B;  /* sltu $26, $27, $26 */

    /* beqz k0, not_ours */
    /* offset will be patched after we know the target */
    int beqz1_idx = n;
    stub[n++] = 0x13400000;  /* beqz $26, +0 (placeholder) */

    /* lui k0, 0x2000 (delay slot: lower bound = 0x20000000) */
    stub[n++] = 0x3C1A2000;  /* lui $26, 0x2000 */

    /* sltu k0, k1, k0  (k0 = (BadVAddr < 0x20000000) ? 1 : 0) */
    stub[n++] = 0x037AD02B;  /* sltu $26, $27, $26 */

    /* bnez k0, not_ours */
    int bnez_idx = n;
    stub[n++] = 0x17400000;  /* bnez $26, +0 (placeholder) */

    /* nop (delay slot) */
    stub[n++] = 0x00000000;

    /* === OUR TLB MISS === */
    /* mfc0 k0, EPC ($14) */
    stub[n++] = 0x401A7000;  /* mfc0 $26, $14 */

    /* lui k1, 0x7000 (scratchpad base) */
    stub[n++] = 0x3C1B7000;  /* lui $27, 0x7000 */

    /* sw k0, SP_FAULT_EPC(k1) */
    stub[n++] = 0xAF7A3FE0;  /* sw $26, 0x3FE0($27) */

    /* lw k0, 0(k0)  — load faulting instruction */
    stub[n++] = 0x8F5A0000;  /* lw $26, 0($26) */

    /* sw k0, SP_FAULT_INSN(k1) */
    stub[n++] = 0xAF7A3FE4;  /* sw $26, 0x3FE4($27) */

    /* lw k0, SP_TRAMPOLINE_ADDR(k1) */
    stub[n++] = 0x8F7A3FE8;  /* lw $26, 0x3FE8($27) */

    /* mtc0 k0, EPC ($14) */
    stub[n++] = 0x409A7000;  /* mtc0 $26, $14 */

    /* sync.p (R5900 pipeline sync) */
    stub[n++] = 0x0000000F;  /* sync (sync.p = sync 0x10, but sync works too) */

    /* eret */
    stub[n++] = 0x42000018;  /* eret */

    /* === NOT OURS === */
    int not_ours_idx = n;

    /* lui k1, 0x7000 */
    stub[n++] = 0x3C1B7000;

    /* lw k0, SP_ORIG_HANDLER(k1) */
    stub[n++] = 0x8F7A3FEC;  /* lw $26, 0x3FEC($27) */

    /* jr k0 */
    stub[n++] = 0x03400008;  /* jr $26 */

    /* nop (delay slot) */
    stub[n++] = 0x00000000;

    /* Patch branch offsets */
    {
        int16_t off1 = (int16_t)(not_ours_idx - beqz1_idx - 1);
        stub[beqz1_idx] = (stub[beqz1_idx] & 0xFFFF0000) | (off1 & 0xFFFF);

        int16_t off2 = (int16_t)(not_ours_idx - bnez_idx - 1);
        stub[bnez_idx] = (stub[bnez_idx] & 0xFFFF0000) | (off2 & 0xFFFF);
    }

    /* Write stub to exception vector */
    memcpy((void *)0x80000000, stub, n * 4);

    /* Flush caches: writeback D-cache, invalidate I-cache */
    FlushCache(0);
    FlushCache(2);

    printf("  TLB handler installed at 0x80000000 (%d instructions)\n", n);
}

/* ================================================================
 *  Setup_PSX_TLB: Called once after psx_ram allocation.
 *
 *  1. Creates a TLB entry mapping psx_ram at VA 0x20000000
 *  2. Installs the TLB refill handler
 *  3. Verifies the mapping
 * ================================================================ */
void Setup_PSX_TLB(void)
{
    printf("Setting up PSX TLB mapping...\n");

    /* ---- 1. RAM: VA 0x20000000, 1MB pages (1 entry = 2MB) ---- */
    {
        uint32_t phys = (uint32_t)psx_ram & 0x1FFFFFFF;
        uint32_t page_mask = 0x001FE000;   /* 1MB page size */
        uint32_t entry_hi  = PSX_TLB_BASE; /* VPN2 + ASID=0 */
        uint32_t pfn0      = phys >> 12;
        uint32_t pfn1      = (phys + 0x100000) >> 12;
        /* EntryLo: PFN | C=3 (cacheable) | D=1 (dirty) | V=1 (valid) | G=1 (global) */
        uint32_t entry_lo0 = (pfn0 << 6) | (3 << 3) | (1 << 2) | (1 << 1) | 1;
        uint32_t entry_lo1 = (pfn1 << 6) | (3 << 3) | (1 << 2) | (1 << 1) | 1;

        int idx = PutTLBEntry(page_mask, entry_hi, entry_lo0, entry_lo1);
        printf("  RAM  TLB[%d]: VA 0x%08lX -> PA 0x%08lX (2MB, cacheable)\n",
               idx, (unsigned long)PSX_TLB_BASE, (unsigned long)phys);

        /* Verify mapping */
        *(volatile uint32_t *)psx_ram = 0xDEADBEEF;
        FlushCache(0);
        volatile uint32_t *tlb = (volatile uint32_t *)PSX_TLB_BASE;
        uint32_t test = *tlb;
        if (test != 0xDEADBEEF)
        {
            printf("  ERROR: RAM TLB mapping failed! Read 0x%08lX\n",
                   (unsigned long)test);
            *(volatile uint32_t *)psx_ram = 0;
            return;
        }
        *tlb = 0xCAFEBABE;
        FlushCache(0);
        if (*(volatile uint32_t *)psx_ram != 0xCAFEBABE)
        {
            printf("  WARNING: RAM TLB write-through failed!\n");
        }
        *(volatile uint32_t *)psx_ram = 0;
        printf("  RAM  TLB: verified OK\n");
    }

    /* ---- 2. Scratchpad: VA 0x3F800000, 4KB pages ---- */
    /* After mask+add: physical 0x1F800000 + 0x20000000 = VA 0x3F800000      */
    /* Even page (0x3F800000): scratchpad_buf, V=1, D=1, C=3, G=1            */
    /* Odd  page (0x3F801000): INVALID (V=0) — HW I/O, let it miss to trap   */
    {
        uint32_t sp_phys   = (uint32_t)scratchpad_buf & 0x1FFFFFFF;
        uint32_t sp_va     = PSX_TLB_BASE + 0x1F800000; /* 0x3F800000 */
        uint32_t page_mask = 0x00000000;   /* 4KB pages */
        uint32_t entry_hi  = sp_va;        /* VPN2 */
        uint32_t pfn0      = sp_phys >> 12;
        /* Even: PFN | C=3 | D=1 | V=1 | G=1 */
        uint32_t entry_lo0 = (pfn0 << 6) | (3 << 3) | (1 << 2) | (1 << 1) | 1;
        /* Odd: INVALID (V=0, G=1 to keep global) — forces TLB miss for HW I/O */
        uint32_t entry_lo1 = 1; /* G=1 only */

        int idx = PutTLBEntry(page_mask, entry_hi, entry_lo0, entry_lo1);
        printf("  SP   TLB[%d]: VA 0x%08lX -> PA 0x%08lX (4KB, cacheable)\n",
               idx, (unsigned long)sp_va, (unsigned long)sp_phys);
        printf("            : VA 0x%08lX INVALID (HW I/O trap)\n",
               (unsigned long)(sp_va + 0x1000));

        /* Verify */
        scratchpad_buf[0] = 0xAB;
        FlushCache(0);
        volatile uint8_t *sp_test = (volatile uint8_t *)sp_va;
        if (*sp_test == 0xAB)
        {
            printf("  SP   TLB: verified OK\n");
            scratchpad_buf[0] = 0;
        }
        else
        {
            printf("  WARNING: SP TLB verification failed (read 0x%02X)\n", *sp_test);
        }
    }

    /* ---- 3. BIOS: VA 0x3FC00000, 256KB pages (1 entry = 512KB) ---- */
    /* After mask+add: physical 0x1FC00000 + 0x20000000 = VA 0x3FC00000       */
    {
        uint32_t bios_phys = (uint32_t)psx_bios & 0x1FFFFFFF;
        uint32_t bios_va   = PSX_TLB_BASE + 0x1FC00000; /* 0x3FC00000 */
        uint32_t page_mask = 0x0007E000;   /* 256KB pages */
        uint32_t entry_hi  = bios_va;      /* VPN2 */
        uint32_t pfn0      = bios_phys >> 12;
        uint32_t pfn1      = (bios_phys + 0x40000) >> 12;
        /* Both pages: PFN | C=3 | D=1 | V=1 | G=1 */
        uint32_t entry_lo0 = (pfn0 << 6) | (3 << 3) | (1 << 2) | (1 << 1) | 1;
        uint32_t entry_lo1 = (pfn1 << 6) | (3 << 3) | (1 << 2) | (1 << 1) | 1;

        int idx = PutTLBEntry(page_mask, entry_hi, entry_lo0, entry_lo1);
        printf("  BIOS TLB[%d]: VA 0x%08lX -> PA 0x%08lX (512KB, cacheable)\n",
               idx, (unsigned long)bios_va, (unsigned long)bios_phys);
    }

    /* Install exception handler (covers VA 0x20000000-0x3FFFFFFF) */
    install_tlb_handler();

    /* Export base address for JIT */
    psx_tlb_base = PSX_TLB_BASE;

    printf("  TLB fast-path active: JIT S1 = 0x%08lX\n", (unsigned long)psx_tlb_base);
    printf("  TLB entries used: 3 of 48 (RAM + Scratchpad + BIOS)\n");
}
