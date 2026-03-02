#ifndef SUPERPSX_H
#define SUPERPSX_H

#include <inttypes.h>
#include <stdio.h>
#include <setjmp.h>
#include "scheduler.h"

/*=== Debug logging macro ===*/
#ifdef ENABLE_DEBUG_LOG
#define DLOG(fmt, ...) printf("[" LOG_TAG "] " fmt, ##__VA_ARGS__)
#define DLOG_RAW(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define DLOG(...) \
    do            \
    {             \
    } while (0)
#define DLOG_RAW(...) \
    do                \
    {                 \
    } while (0)
#endif

/*=== CPU State ===*/
typedef struct
{
    uint32_t regs[32];            /* 0x00: GPR */
    uint32_t pc;                  /* 0x80: Program Counter */
    uint32_t hi;                  /* 0x84 */
    uint32_t lo;                  /* 0x88 */
    uint32_t cop0[32];            /* 0x8C: COP0 registers */
    uint32_t cp2_data[32];        /* 0x10C: GTE Data Registers (V0, V1, V2, etc.) */
    uint32_t cp2_ctrl[32];        /* 0x18C: GTE Control Registers (Matrices, etc.) */
    uint32_t current_pc;          /* PC of the currently executing instruction (for exceptions) */
    uint32_t load_delay_reg;      /* Load delay slot: target register index (0=none) */
    uint32_t load_delay_val;      /* Load delay slot: pending value */
    uint32_t i_stat;              /* Interrupt Status Register */
    uint32_t i_mask;              /* Interrupt Mask Register */
    uint32_t block_aborted;       /* Set by PSX_Exception mid-block; checked by JIT */
    uint32_t branch_cond;         /* Scratch: branch condition saved across delay slot */
    uint32_t initial_cycles_left; /* Used to compute elapsed cycles during JIT execution */
    uint32_t cycles_left;         /* Maintained by JIT, sync'd to cpu on C calls */
} R3000CPU;

/* Struct offsets for asm code generation */
#define CPU_REG(n) ((n) * 4)
#define CPU_PC (32 * 4)
#define CPU_HI (32 * 4 + 4)
#define CPU_LO (32 * 4 + 8)
#define CPU_COP0(n) (32 * 4 + 12 + (n) * 4)
#define CPU_CP2_DATA(n) (32 * 4 + 12 + 32 * 4 + (n) * 4)
#define CPU_CP2_CTRL(n) (32 * 4 + 12 + 32 * 4 + 32 * 4 + (n) * 4)
#define CPU_CURRENT_PC (32 * 4 + 12 + 32 * 4 + 32 * 4 + 32 * 4)
#define CPU_LOAD_DELAY_REG (CPU_CURRENT_PC + 4)
#define CPU_LOAD_DELAY_VAL (CPU_CURRENT_PC + 8)
#define CPU_I_STAT (CPU_LOAD_DELAY_VAL + 4)
#define CPU_I_MASK (CPU_I_STAT + 4)
#define CPU_BLOCK_ABORTED (CPU_I_MASK + 4)
#define CPU_BRANCH_COND (CPU_BLOCK_ABORTED + 4)
#define CPU_INITIAL_CYCLES_LEFT (CPU_BRANCH_COND + 4)
#define CPU_CYCLES_LEFT (CPU_INITIAL_CYCLES_LEFT + 4)

/* COP0 register indices */
#define PSX_COP0_SR 12
#define PSX_COP0_CAUSE 13
#define PSX_COP0_EPC 14
#define PSX_COP0_PRID 15
#define PSX_COP0_BADVADDR 8

extern R3000CPU cpu;

/*=== Memory ===*/
#define PSX_RAM_SIZE 0x200000 /* 2MB */
#define PSX_BIOS_SIZE 0x80000 /* 512KB */
/* Scratchpad (1KB) */
#define PSX_SCRATCHPAD_SIZE 1024

extern uint8_t *psx_ram;
extern uint8_t *psx_bios;
extern uint8_t scratchpad_buf[];
extern uint8_t **mem_lut;
extern uint32_t psx_tlb_base; /* 0x20000000 if TLB active, 0 otherwise */

#define MEM_LUT_SIZE 65536

void Init_Memory(void);
void Init_MemoryLUT(void);
void Setup_PSX_TLB(void);
int Load_BIOS(const char *filename);
uint32_t ReadWord(uint32_t addr);
uint16_t ReadHalf(uint32_t addr);
uint8_t ReadByte(uint32_t addr);
void WriteWord(uint32_t addr, uint32_t data);
void WriteHalf(uint32_t addr, uint16_t data);
void WriteByte(uint32_t addr, uint8_t data);

/*=== Hardware ===*/
uint32_t ReadHardware(uint32_t phys);
/* size: access width in bytes (1, 2, or 4).  Used by the SPU handler to
 * decide whether to write one or two consecutive 16-bit registers. */
void WriteHardware(uint32_t phys, uint32_t data, int size);
void SignalInterrupt(uint32_t irq);
void Init_Interrupts(void);
static inline int CheckInterrupts(void)
{
    return (cpu.i_stat & cpu.i_mask & 0x7FF);
}
void UpdateTimers(uint32_t cycles);

/*=== Dynarec ===*/
void Init_Dynarec(void);
void Run_CPU(void);
void dynarec_print_stats(void);
void GTE_Execute(uint32_t opcode, R3000CPU *cpu);
uint32_t GTE_ReadData(R3000CPU *cpu, int reg);
void GTE_WriteData(R3000CPU *cpu, int reg, uint32_t val);
uint32_t GTE_ReadCtrl(R3000CPU *cpu, int reg);
void GTE_WriteCtrl(R3000CPU *cpu, int reg, uint32_t val);
void GTE_VBlankUpdate(void);

/* VU0 fast-path state (flag-read detection) */
extern int gte_flag_read_count;
extern int gte_use_vu0;

/* GTE inline command wrappers (skip GTE_Execute dispatcher) */
void GTE_Inline_RTPS(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_NCLIP(R3000CPU *cpu);
void GTE_Inline_OP(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_DPCS(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_INTPL(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_MVMVA(R3000CPU *cpu, uint32_t packed); /* packed = sf|(lm<<1)|(mx<<2)|(v<<4)|(cv<<6) */
void GTE_Inline_NCDS(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_CDP(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_NCDT(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_NCCS(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_CC(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_NCS(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_NCT(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_SQR(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_DCPL(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_DPCT(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_AVSZ3(R3000CPU *cpu);
void GTE_Inline_AVSZ4(R3000CPU *cpu);
void GTE_Inline_RTPT(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_GPF(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_GPL(R3000CPU *cpu, int sf, int lm);
void GTE_Inline_NCCT(R3000CPU *cpu, int sf, int lm);

/*=== CPU Helper Functions (called from dynarec) ===*/
uint32_t Helper_LWL(uint32_t addr, uint32_t cur_rt);
uint32_t Helper_LWR(uint32_t addr, uint32_t cur_rt);
void Helper_SWL(uint32_t addr, uint32_t rt_val);
void Helper_SWR(uint32_t addr, uint32_t rt_val);
void Helper_DIV(int32_t rs, int32_t rt, uint32_t *lo_out, uint32_t *hi_out);
void Helper_DIVU(uint32_t rs, uint32_t rt, uint32_t *lo_out, uint32_t *hi_out);

/*=== CPU / COP0 ===*/
void Init_CPU(void);
void PSX_Exception(uint32_t cause_code);
void Handle_Syscall(void);
void Helper_Syscall_Exception(uint32_t pc);
void Helper_Break_Exception(uint32_t pc);
void Helper_CU_Exception(uint32_t pc, uint32_t cop_num);
void Helper_ADD(uint32_t rs_val, uint32_t rt_val, uint32_t rd, uint32_t pc);
void Helper_SUB(uint32_t rs_val, uint32_t rt_val, uint32_t rd, uint32_t pc);
void Helper_ADDI(uint32_t rs_val, uint32_t imm_sext, uint32_t rt, uint32_t pc);
void Helper_Overflow_Exception(void *unused);

/*=== Exception support for dynarec ===*/
extern jmp_buf psx_block_jmp;
extern volatile int psx_block_exception;
extern uint32_t psx_abort_pc;

/*=== CD-ROM ===*/
void CDROM_Init(void);
void CDROM_InsertDisc(void);
uint32_t CDROM_Read(uint32_t addr);
void CDROM_Write(uint32_t addr, uint32_t data);
/* Set by scheduler when CD-ROM int_flag is active and signal delay expired.
 * Checked inline in dynarec loop for cheap level-triggered re-assertion. */
extern uint8_t cdrom_irq_active;

/* SIO (controller) delayed IRQ7 support.
 * The PSX BIOS kernel acknowledges any old IRQ7 ~100 cycles after sending a
 * byte, then waits for the new IRQ7.  Firing IRQ7 immediately would cause the
 * acknowledge to clear the pending IRQ before the kernel polls for it.
 * Set to the global_cycles deadline at which IRQ7 should actually fire;
 * 0 means no pending SIO IRQ. */
extern volatile uint64_t sio_irq_delay_cycle;

/* GPU (IRQ1) deferred interrupt support.
 * On real PSX hardware the GPU command FIFO is processed asynchronously:
 * writing GP0(1Fh) puts the "Interrupt Request" command in the FIFO, and the
 * interrupt only reaches I_STAT bit 1 AFTER the GPU has processed it - which
 * is several hundred CPU cycles after the write.  Firing IRQ1 synchronously
 * (inside the SW handler) causes tests that read I_STAT immediately after the
 * GP0 write to see bit 1 set when they expect 0.
 * Set to the global_cycles deadline at which IRQ1 should fire; 0 = no pending. */
extern volatile uint64_t gpu_irq_delay_cycle;
void CDROM_ScheduleEvent(void);
uint32_t CDROM_ReadDataFIFO(uint8_t *dst, uint32_t count);

/*=== Timer Scheduler ===*/
void Timer_ScheduleAll(void);

/*=== Graphics ===*/
void Init_Graphics(void);
void GPU_WriteGP0(uint32_t data);
void GPU_WriteGP1(uint32_t data);
uint32_t GPU_Read(void);
uint32_t GPU_ReadStatus(void);
void GPU_VBlank(void);
void GPU_Flush(void);
void GPU_DMA2(uint32_t madr, uint32_t bcr, uint32_t chcr);

/*=== Main ===*/
void Init_SuperPSX(void);

/* Write argc/argv for the PS-X EXE into the PSX scratchpad so the
 * loaded executable can read them from 0x1F800000 (argc) and the
 * pointer table starting at 0x1F800004, with strings at 0x1F800044.
 * args: array of C strings, argLen: number of strings. */
void PSX_SetArgs(const char **args, int argLen);

/* Filename of PS-X EXE to load (defaults to "test.exe").
 * Can be overridden from `main` using command-line arguments. */
extern const char *psx_exe_filename;

/* Boot mode: 0 = PS-X EXE, 1 = ISO disc */
#define BOOT_MODE_EXE 0
#define BOOT_MODE_ISO 1
extern int psx_boot_mode;

#endif
