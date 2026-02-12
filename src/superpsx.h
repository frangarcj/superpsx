#ifndef SUPERPSX_H
#define SUPERPSX_H

#include <tamtypes.h>

/*=== CPU State ===*/
typedef struct {
    u32 regs[32];       /* 0x00: GPR */
    u32 pc;             /* 0x80: Program Counter */
    u32 hi;             /* 0x84 */
    u32 lo;             /* 0x88 */
    u32 cop0[32];       /* 0x8C: COP0 registers */
} R3000CPU;

/* Struct offsets for asm code generation */
#define CPU_REG(n)    ((n) * 4)
#define CPU_PC        (32 * 4)
#define CPU_HI        (32 * 4 + 4)
#define CPU_LO        (32 * 4 + 8)
#define CPU_COP0(n)   (32 * 4 + 12 + (n) * 4)

/* COP0 register indices */
#define COP0_SR       12
#define COP0_CAUSE    13
#define COP0_EPC      14
#define COP0_PRID     15
#define COP0_BADVADDR 8

extern R3000CPU cpu;

/*=== Memory ===*/
#define PSX_RAM_SIZE   0x200000   /* 2MB */
#define PSX_BIOS_SIZE  0x80000    /* 512KB */

extern u8 *psx_ram;
extern u8 *psx_bios;

void Init_Memory(void);
int  Load_BIOS(const char *filename);
u32  ReadWord(u32 addr);
u16  ReadHalf(u32 addr);
u8   ReadByte(u32 addr);
void WriteWord(u32 addr, u32 data);
void WriteHalf(u32 addr, u16 data);
void WriteByte(u32 addr, u8 data);

/*=== Hardware ===*/
u32  ReadHardware(u32 addr);
void WriteHardware(u32 addr, u32 data);

/*=== Dynarec ===*/
void Init_Dynarec(void);
void Run_CPU(void);

/*=== CPU / COP0 ===*/
void Init_CPU(void);
void PSX_Exception(u32 cause_code);
void Handle_Syscall(void);

/*=== Graphics ===*/
void Init_Graphics(void);

/*=== Main ===*/
void Init_SuperPSX(void);

#endif
