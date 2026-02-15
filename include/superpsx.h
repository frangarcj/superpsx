#ifndef SUPERPSX_H
#define SUPERPSX_H

#include <tamtypes.h>
#include <setjmp.h>

/*=== CPU State ===*/
typedef struct
{
    u32 regs[32];       /* 0x00: GPR */
    u32 pc;             /* 0x80: Program Counter */
    u32 hi;             /* 0x84 */
    u32 lo;             /* 0x88 */
    u32 cop0[32];       /* 0x8C: COP0 registers */
    u32 cp2_data[32];   /* 0x10C: GTE Data Registers (V0, V1, V2, etc.) */
    u32 cp2_ctrl[32];   /* 0x18C: GTE Control Registers (Matrices, etc.) */
    u32 current_pc;     /* PC of the currently executing instruction (for exceptions) */
    u32 load_delay_reg; /* Load delay slot: target register index (0=none) */
    u32 load_delay_val; /* Load delay slot: pending value */
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

extern u8 *psx_ram;
extern u8 *psx_bios;

void Init_Memory(void);
int Load_BIOS(const char *filename);
u32 ReadWord(u32 addr);
u16 ReadHalf(u32 addr);
u8 ReadByte(u32 addr);
void WriteWord(u32 addr, u32 data);
void WriteHalf(u32 addr, u16 data);
void WriteByte(u32 addr, u8 data);

/*=== Hardware ===*/
u32 ReadHardware(u32 addr);
void WriteHardware(u32 addr, u32 data);
void SignalInterrupt(u32 irq);
int CheckInterrupts(void);
void Init_Interrupts(void);
void UpdateTimers(u32 cycles);

/*=== Dynarec ===*/
void Init_Dynarec(void);
void Run_CPU(void);
void GTE_Execute(u32 opcode, R3000CPU *cpu);
u32 GTE_ReadData(R3000CPU *cpu, int reg);
void GTE_WriteData(R3000CPU *cpu, int reg, u32 val);
u32 GTE_ReadCtrl(R3000CPU *cpu, int reg);
void GTE_WriteCtrl(R3000CPU *cpu, int reg, u32 val);

/*=== CPU Helper Functions (called from dynarec) ===*/
u32 Helper_LWL(u32 addr, u32 cur_rt);
u32 Helper_LWR(u32 addr, u32 cur_rt);
void Helper_SWL(u32 addr, u32 rt_val);
void Helper_SWR(u32 addr, u32 rt_val);
void Helper_DIV(s32 rs, s32 rt, u32 *lo_out, u32 *hi_out);
void Helper_DIVU(u32 rs, u32 rt, u32 *lo_out, u32 *hi_out);

/*=== CPU / COP0 ===*/
void Init_CPU(void);
void PSX_Exception(u32 cause_code);
void Handle_Syscall(void);
void Helper_Syscall_Exception(u32 pc);
void Helper_Break_Exception(u32 pc);
void Helper_ADD(u32 rs_val, u32 rt_val, u32 rd, u32 pc);
void Helper_SUB(u32 rs_val, u32 rt_val, u32 rd, u32 pc);
void Helper_ADDI(u32 rs_val, u32 imm_sext, u32 rt, u32 pc);

/*=== Exception support for dynarec ===*/
extern jmp_buf psx_block_jmp;
extern volatile int psx_block_exception;

/*=== CD-ROM ===*/
void CDROM_Init(void);
u32 CDROM_Read(u32 addr);
void CDROM_Write(u32 addr, u32 data);

/*=== Graphics ===*/
void Init_Graphics(void);
void GPU_WriteGP0(u32 data);
void GPU_WriteGP1(u32 data);
u32 GPU_Read(void);
u32 GPU_ReadStatus(void);
void GPU_VBlank(void);
void GPU_Flush(void);
void GPU_DMA2(u32 madr, u32 bcr, u32 chcr);

/*=== Main ===*/
void Init_SuperPSX(void);

/* Filename of PS-X EXE to load (defaults to "test.exe").
 * Can be overridden from `main` using command-line arguments. */
extern const char *psx_exe_filename;

#endif
