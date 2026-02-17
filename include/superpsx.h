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
#define DLOG(...) do {} while (0)
#define DLOG_RAW(...) do {} while (0)
#endif

/*=== CPU State ===*/
typedef struct
{
    uint32_t regs[32];       /* 0x00: GPR */
    uint32_t pc;             /* 0x80: Program Counter */
    uint32_t hi;             /* 0x84 */
    uint32_t lo;             /* 0x88 */
    uint32_t cop0[32];       /* 0x8C: COP0 registers */
    uint32_t cp2_data[32];   /* 0x10C: GTE Data Registers (V0, V1, V2, etc.) */
    uint32_t cp2_ctrl[32];   /* 0x18C: GTE Control Registers (Matrices, etc.) */
    uint32_t current_pc;     /* PC of the currently executing instruction (for exceptions) */
    uint32_t load_delay_reg; /* Load delay slot: target register index (0=none) */
    uint32_t load_delay_val; /* Load delay slot: pending value */
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

extern uint8_t *psx_ram;
extern uint8_t *psx_bios;

void Init_Memory(void);
int Load_BIOS(const char *filename);
uint32_t ReadWord(uint32_t addr);
uint16_t ReadHalf(uint32_t addr);
uint8_t ReadByte(uint32_t addr);
void WriteWord(uint32_t addr, uint32_t data);
void WriteHalf(uint32_t addr, uint16_t data);
void WriteByte(uint32_t addr, uint8_t data);

/*=== Hardware ===*/
uint32_t ReadHardware(uint32_t addr);
void WriteHardware(uint32_t addr, uint32_t data);
void SignalInterrupt(uint32_t irq);
int CheckInterrupts(void);
void Init_Interrupts(void);
void UpdateTimers(uint32_t cycles);

/*=== Dynarec ===*/
void Init_Dynarec(void);
void Run_CPU(void);
void GTE_Execute(uint32_t opcode, R3000CPU *cpu);
uint32_t GTE_ReadData(R3000CPU *cpu, int reg);
void GTE_WriteData(R3000CPU *cpu, int reg, uint32_t val);
uint32_t GTE_ReadCtrl(R3000CPU *cpu, int reg);
void GTE_WriteCtrl(R3000CPU *cpu, int reg, uint32_t val);

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

/*=== Exception support for dynarec ===*/
extern jmp_buf psx_block_jmp;
extern volatile int psx_block_exception;

/*=== CD-ROM ===*/
void CDROM_Init(void);
uint32_t CDROM_Read(uint32_t addr);
void CDROM_Write(uint32_t addr, uint32_t data);
void CDROM_Update(uint32_t cycles);
void CDROM_ScheduleEvent(void);

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

/* Filename of PS-X EXE to load (defaults to "test.exe").
 * Can be overridden from `main` using command-line arguments. */
extern const char *psx_exe_filename;

#endif
