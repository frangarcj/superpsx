#include <tamtypes.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "superpsx.h"

/* PSX Memory Map:
 *   0x00000000-0x001FFFFF: kuseg RAM (2MB, mirrored)
 *   0x80000000-0x801FFFFF: kseg0 RAM (cached)
 *   0xA0000000-0xA01FFFFF: kseg1 RAM (uncached)
 *   0x1F800000-0x1F8003FF: Scratchpad (1KB)
 *   0x1F801000-0x1F802FFF: Hardware Registers
 *   0xBFC00000-0xBFC7FFFF: BIOS (512KB)
 *   0x9FC00000-0x9FC7FFFF: BIOS (cached mirror)
 *   0xFFFE0130:            Cache Control
 */

u8 *psx_ram;
u8 *psx_bios;
static u8 scratchpad_buf[1024] __attribute__((aligned(128)));

/* Memory control regs the BIOS writes during init */
static u32 mem_ctrl[16];
static u32 ram_size_reg = 0x00000B88;
static u32 cache_ctrl = 0;

void Init_Memory(void) {
    printf("Initializing Memory Map...\n");

    /* Allocate PSX RAM and BIOS dynamically to avoid ELF overlap */
    psx_ram = (u8 *)memalign(128, PSX_RAM_SIZE);
    psx_bios = (u8 *)memalign(128, PSX_BIOS_SIZE);

    if (!psx_ram || !psx_bios) {
        printf("  ERROR: Failed to allocate PSX memory!\n");
        return;
    }

    /* Clear PSX RAM */
    memset(psx_ram, 0, PSX_RAM_SIZE);
    memset(scratchpad_buf, 0, sizeof(scratchpad_buf));
    memset(mem_ctrl, 0, sizeof(mem_ctrl));

    printf("  RAM:  %p (2MB)\n", psx_ram);
    printf("  BIOS: %p (512KB)\n", psx_bios);
}

int Load_BIOS(const char *filename) {
    printf("Loading BIOS from %s...\n", filename);
    FILE *f = fopen(filename, "rb");
    if (!f) {
        printf("  ERROR: Cannot open file\n");
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    printf("  BIOS size: %ld bytes\n", size);
    if (size > PSX_BIOS_SIZE) {
        fclose(f);
        return -2;
    }
    size_t rd = fread(psx_bios, 1, size, f);
    fclose(f);
    printf("  BIOS loaded: %lu bytes at %p\n", (unsigned long)rd, psx_bios);

    /* Print first 4 instructions for verification */
    u32 *bios32 = (u32 *)psx_bios;
    int i;
    for (i = 0; i < 4; i++) {
        printf("  BIOS[%d]: 0x%08X\n", i, (unsigned)bios32[i]);
    }
    return 0;
}

/* ---- Address translation ---- */
static inline u32 translate_addr(u32 addr) {
    /* Strip segment bits to get physical address */
    return addr & 0x1FFFFFFF;
}

/* ---- Read functions ---- */
u8 ReadByte(u32 addr) {
    u32 phys = translate_addr(addr);

    if (phys < PSX_RAM_SIZE)
        return psx_ram[phys];
    if (phys >= 0x1FC00000 && phys < 0x1FC00000 + PSX_BIOS_SIZE)
        return psx_bios[phys - 0x1FC00000];
    if (phys >= 0x1F800000 && phys < 0x1F800400)
        return scratchpad_buf[phys - 0x1F800000];
    if (phys >= 0x1F801000 && phys < 0x1F803000)
        return (u8)ReadHardware(addr);
    return 0;
}

u16 ReadHalf(u32 addr) {
    u32 phys = translate_addr(addr);

    if (phys < PSX_RAM_SIZE)
        return *(u16 *)(psx_ram + phys);
    if (phys >= 0x1FC00000 && phys < 0x1FC00000 + PSX_BIOS_SIZE)
        return *(u16 *)(psx_bios + (phys - 0x1FC00000));
    if (phys >= 0x1F800000 && phys < 0x1F800400)
        return *(u16 *)(scratchpad_buf + (phys - 0x1F800000));
    if (phys >= 0x1F801000 && phys < 0x1F803000)
        return (u16)ReadHardware(addr);
    return 0;
}

u32 ReadWord(u32 addr) {
    u32 phys = translate_addr(addr);

    if (phys < PSX_RAM_SIZE)
        return *(u32 *)(psx_ram + phys);
    if (phys >= 0x1FC00000 && phys < 0x1FC00000 + PSX_BIOS_SIZE)
        return *(u32 *)(psx_bios + (phys - 0x1FC00000));
    if (phys >= 0x1F800000 && phys < 0x1F800400)
        return *(u32 *)(scratchpad_buf + (phys - 0x1F800000));
    if (phys >= 0x1F801000 && phys < 0x1F803000)
        return ReadHardware(addr);
    /* Memory control registers */
    if (phys >= 0x1F801000 && phys < 0x1F801024)
        return mem_ctrl[(phys - 0x1F801000) >> 2];
    if (phys == 0x1F801060)
        return ram_size_reg;
    /* Cache control register */
    if (phys == 0x1FFE0130 || addr == 0xFFFE0130)
        return cache_ctrl;
    return 0;
}

/* ---- Write functions ---- */
void WriteByte(u32 addr, u8 data) {
    u32 phys = translate_addr(addr);

    if (phys < PSX_RAM_SIZE) {
        psx_ram[phys] = data;
        return;
    }
    if (phys >= 0x1F800000 && phys < 0x1F800400) {
        scratchpad_buf[phys - 0x1F800000] = data;
        return;
    }
    if (phys >= 0x1F801000 && phys < 0x1F803000) {
        WriteHardware(addr, data);
        return;
    }
}

void WriteHalf(u32 addr, u16 data) {
    u32 phys = translate_addr(addr);

    if (phys < PSX_RAM_SIZE) {
        *(u16 *)(psx_ram + phys) = data;
        return;
    }
    if (phys >= 0x1F800000 && phys < 0x1F800400) {
        *(u16 *)(scratchpad_buf + (phys - 0x1F800000)) = data;
        return;
    }
    if (phys >= 0x1F801000 && phys < 0x1F803000) {
        WriteHardware(addr, data);
        return;
    }
}

void WriteWord(u32 addr, u32 data) {
    u32 phys = translate_addr(addr);

    if (phys < PSX_RAM_SIZE) {
        *(u32 *)(psx_ram + phys) = data;
        return;
    }
    if (phys >= 0x1F800000 && phys < 0x1F800400) {
        *(u32 *)(scratchpad_buf + (phys - 0x1F800000)) = data;
        return;
    }
    if (phys >= 0x1F801000 && phys < 0x1F803000) {
        WriteHardware(addr, data);
        return;
    }
    /* Memory control registers */
    if (phys >= 0x1F801000 && phys < 0x1F801024) {
        mem_ctrl[(phys - 0x1F801000) >> 2] = data;
        return;
    }
    if (phys == 0x1F801060) {
        ram_size_reg = data;
        return;
    }
    /* Cache control */
    if (phys == 0x1FFE0130 || addr == 0xFFFE0130) {
        cache_ctrl = data;
        return;
    }
    /* Ignore writes to BIOS ROM */
}
