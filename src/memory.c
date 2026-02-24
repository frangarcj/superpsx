#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <fcntl.h>
#include "superpsx.h"

#define LOG_TAG "MEM"

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

uint8_t *psx_ram;
uint8_t *psx_bios;
uint8_t scratchpad_buf[1024] __attribute__((aligned(128)));

/*
 * Memory LUT: 64KB virtual address pages.
 * 65536 entries × 4 bytes = 256KB.
 * Each entry is a host pointer to the start of the mapped 64KB region,
 * or NULL for IO/unmapped pages that require the slow C helper path.
 * Indexed by (virtual_address >> 16).
 * Dynamically allocated to avoid BSS mapping issues on PS2.
 */
uint8_t **mem_lut;

void Init_MemoryLUT(void)
{
    int page;
    mem_lut = (uint8_t **)memalign(128, MEM_LUT_SIZE * sizeof(uint8_t *));
    if (!mem_lut)
    {
        printf("  ERROR: Failed to allocate mem_lut!\n");
        return;
    }
    memset(mem_lut, 0, MEM_LUT_SIZE * sizeof(uint8_t *));

    /* RAM pages: 32 × 64KB = 2MB */
    for (page = 0; page < 0x20; page++)
    {
        uint8_t *base = psx_ram + page * 0x10000;
        /* kuseg: 0x00000000-0x001FFFFF */
        mem_lut[0x0000 + page] = base;
        /* kseg0: 0x80000000-0x801FFFFF */
        mem_lut[0x8000 + page] = base;
        /* kseg1: 0xA0000000-0xA01FFFFF */
        mem_lut[0xA000 + page] = base;
        /* kuseg RAM mirrors (2-8MB) */
        mem_lut[0x0020 + page] = base;
        mem_lut[0x0040 + page] = base;
        mem_lut[0x0060 + page] = base;
        /* kseg0 RAM mirrors */
        mem_lut[0x8020 + page] = base;
        mem_lut[0x8040 + page] = base;
        mem_lut[0x8060 + page] = base;
        /* kseg1 RAM mirrors */
        mem_lut[0xA020 + page] = base;
        mem_lut[0xA040 + page] = base;
        mem_lut[0xA060 + page] = base;
    }

    /* BIOS pages: 8 × 64KB = 512KB */
    for (page = 0; page < 8; page++)
    {
        uint8_t *base = psx_bios + page * 0x10000;
        /* kuseg: 0x1FC00000-0x1FC7FFFF */
        mem_lut[0x1FC0 + page] = base;
        /* kseg0: 0x9FC00000-0x9FC7FFFF */
        mem_lut[0x9FC0 + page] = base;
        /* kseg1: 0xBFC00000-0xBFC7FFFF */
        mem_lut[0xBFC0 + page] = base;
    }

    /* Note: scratchpad (0x1F800000) and IO regs (0x1F801000) share
     * the same 64KB page, so it stays NULL → slow path via C helpers. */

    printf("  Memory LUT at %p (65536 entries, %u KB)\n",
           (void *)mem_lut, (unsigned)(MEM_LUT_SIZE * sizeof(uint8_t *) / 1024));
    printf("  LUT[0x8000]=%p LUT[0xBFC0]=%p\n",
           (void *)mem_lut[0x8000], (void *)mem_lut[0xBFC0]);
}

/* Memory control regs the BIOS writes during init */
static uint32_t mem_ctrl[16];
static uint32_t ram_size_reg = 0x00000B88;
static uint32_t cache_ctrl = 0;

void Init_Memory(void)
{
    printf("Initializing Memory Map...\n");

    /* Allocate PSX RAM and BIOS dynamically to avoid ELF overlap */
    psx_ram = (uint8_t *)memalign(128, PSX_RAM_SIZE);
    psx_bios = (uint8_t *)memalign(128, PSX_BIOS_SIZE);

    if (!psx_ram || !psx_bios)
    {
        printf("  ERROR: Failed to allocate PSX memory!\n");
        return;
    }

    /* Clear PSX RAM */
    memset(psx_ram, 0, PSX_RAM_SIZE);
    memset(scratchpad_buf, 0, sizeof(scratchpad_buf));
    memset(mem_ctrl, 0, sizeof(mem_ctrl));

    printf("  RAM:  %p (2MB)\n", psx_ram);
    printf("  BIOS: %p (512KB)\n", psx_bios);

    Init_MemoryLUT();
}

#include <dirent.h>

int Load_BIOS_From_ROM(void)
{
    /* PS2 ROM0 starts at 0xBFC00000 */
    uint8_t *rom_base = (uint8_t *)0xBFC00000;

    /* Copy first 512KB directly (contains Reset Vector + TBIN/SBIN) */
    memcpy(psx_bios, rom_base, PSX_BIOS_SIZE);

    /* Verify signature to be sure */
    const char *sig = "Sony Computer Entertainment Inc.";
    int found_sig = 0;
    for (int i = 0; i < PSX_BIOS_SIZE - strlen(sig); i += 4)
    {
        if (memcmp(psx_bios + i, sig, strlen(sig)) == 0)
        {
            found_sig = 1;
            break;
        }
    }

    if (found_sig)
    {
        printf("  Loaded PS1 BIOS from PS2 ROM0 (Sig found).\n");
        return 0;
    }
    else
    {
        printf("  WARNING: No PS1 BIOS signature found in ROM0 copy.\n");
        /* Return 0 anyway as we want to try running it */
        return 0;
    }
}

int Load_BIOS(const char *filename)
{
    printf("Loading BIOS from %s...\n", filename);
    FILE *f = fopen(filename, "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        printf("  BIOS size: %ld bytes\n", size);
        if (size > PSX_BIOS_SIZE)
        {
            printf("  ERROR: BIOS file too large\n");
            fclose(f);
            return -2;
        }
        size_t rd = fread(psx_bios, 1, size, f);
        fclose(f);
        printf("  BIOS loaded: %lu bytes at %p\n", (unsigned long)rd, psx_bios);

        /* Print first 4 instructions for verification */
        uint32_t *bios32 = (uint32_t *)psx_bios;
        int i;
        for (i = 0; i < 4; i++)
        {
            DLOG("  BIOS[%d]: 0x%08X\n", i, (unsigned)bios32[i]);
        }
        return 0;
    }

    printf("  File not found or cannot open. Trying PS2 ROM0...\n");

    /* Try loading from ROM if file failed */
    if (Load_BIOS_From_ROM() == 0)
    {
        printf("  Using PS1 BIOS from PS2 ROM.\n");
        return 0;
    }

    /* Both failed */
    return -1;
}

/* ---- Address translation ---- */
static inline uint32_t translate_addr(uint32_t addr)
{
    /* Strip segment bits to get physical address */
    return addr & 0x1FFFFFFF;
}

/* ---- Read functions ---- */
uint8_t ReadByte(uint32_t addr)
{
    uint32_t phys = translate_addr(addr);

    if (phys < PSX_RAM_SIZE)
        return psx_ram[phys];
    if (phys >= 0x1FC00000 && phys < 0x1FC00000 + PSX_BIOS_SIZE)
        return psx_bios[phys - 0x1FC00000];
    if (phys >= 0x1F800000 && phys < 0x1F800400)
        return scratchpad_buf[phys - 0x1F800000];
    if (phys >= 0x1F801000 && phys < 0x1F803000)
        return (uint8_t)ReadHardware(phys);
    return 0;
}

uint16_t ReadHalf(uint32_t addr)
{
    /* Alignment check: halfword must be 2-byte aligned */
    if (addr & 1)
    {
        cpu.cop0[PSX_COP0_BADVADDR] = addr;
        cpu.pc = cpu.current_pc;
        PSX_Exception(4); /* AdEL */
        return 0;
    }
    uint32_t phys = translate_addr(addr);

    if (phys < PSX_RAM_SIZE)
        return *(uint16_t *)(psx_ram + phys);
    if (phys >= 0x1FC00000 && phys < 0x1FC00000 + PSX_BIOS_SIZE)
        return *(uint16_t *)(psx_bios + (phys - 0x1FC00000));
    if (phys >= 0x1F800000 && phys < 0x1F800400)
        return *(uint16_t *)(scratchpad_buf + (phys - 0x1F800000));
    if (phys >= 0x1F801000 && phys < 0x1F803000)
        return (uint16_t)ReadHardware(phys);
    return 0;
}

uint32_t ReadWord(uint32_t addr)
{
    /* Alignment check: word must be 4-byte aligned */
    if (addr & 3)
    {
        cpu.cop0[PSX_COP0_BADVADDR] = addr;
        cpu.pc = cpu.current_pc;
        PSX_Exception(4); /* AdEL */
        return 0;
    }
    uint32_t phys = translate_addr(addr);

    if (phys < PSX_RAM_SIZE)
        return *(uint32_t *)(psx_ram + phys);
    if (phys >= 0x1FC00000 && phys < 0x1FC00000 + PSX_BIOS_SIZE)
        return *(uint32_t *)(psx_bios + (phys - 0x1FC00000));
    if (phys >= 0x1F800000 && phys < 0x1F800400)
        return *(uint32_t *)(scratchpad_buf + (phys - 0x1F800000));
    if (phys >= 0x1F801000 && phys < 0x1F803000)
        return ReadHardware(phys);
    /* Cache control register */
    if (phys == 0x1FFE0130)
        return cache_ctrl;
    return 0;
}

/* ---- Write functions ---- */
void WriteByte(uint32_t addr, uint8_t data)
{
    /* Cache Isolation: If SR bit 16 is set, ignore writes to KUSEG/KSEG0 */
    if (cpu.cop0[PSX_COP0_SR] & 0x10000)
    {
        if ((addr & 0xE0000000) != 0xA0000000)
        {
            return;
        }
    }

    uint32_t phys = translate_addr(addr);

    if (phys < PSX_RAM_SIZE)
    {
        psx_ram[phys] = data;
        return;
    }
    if (phys >= 0x1F800000 && phys < 0x1F800400)
    {
        scratchpad_buf[phys - 0x1F800000] = data;
        return;
    }
    if (phys >= 0x1F801000 && phys < 0x1F803000)
    {
        WriteHardware(phys, data, 1);
        return;
    }
}

void WriteHalf(uint32_t addr, uint16_t data)
{
    /* Alignment check */
    if (addr & 1)
    {
        cpu.cop0[PSX_COP0_BADVADDR] = addr;
        cpu.pc = cpu.current_pc;
        PSX_Exception(5); /* AdES */
        return;
    }
    /* Cache Isolation */
    if (cpu.cop0[PSX_COP0_SR] & 0x10000)
    {
        if ((addr & 0xE0000000) != 0xA0000000)
        {
            return;
        }
    }

    uint32_t phys = translate_addr(addr);

    if (phys < PSX_RAM_SIZE)
    {
        *(uint16_t *)(psx_ram + phys) = data;
        return;
    }
    if (phys >= 0x1F800000 && phys < 0x1F800400)
    {
        *(uint16_t *)(scratchpad_buf + (phys - 0x1F800000)) = data;
        return;
    }
    if (phys >= 0x1F801000 && phys < 0x1F803000)
    {
        WriteHardware(phys, data, 2);
        return;
    }
}

void WriteWord(uint32_t addr, uint32_t data)
{
    /* Alignment check */
    if (addr & 3)
    {
        cpu.cop0[PSX_COP0_BADVADDR] = addr;
        cpu.pc = cpu.current_pc;
        PSX_Exception(5); /* AdES */
        return;
    }
    /* Cache Isolation */
    if (cpu.cop0[PSX_COP0_SR] & 0x10000)
    {
        if ((addr & 0xE0000000) != 0xA0000000)
        {
            return;
        }
    }

    uint32_t phys = translate_addr(addr);

    if (phys < PSX_RAM_SIZE)
    {
        *(uint32_t *)(psx_ram + phys) = data;
        return;
    }
    if (phys >= 0x1F800000 && phys < 0x1F800400)
    {
        *(uint32_t *)(scratchpad_buf + (phys - 0x1F800000)) = data;
        return;
    }
    if (phys >= 0x1F801000 && phys < 0x1F803000)
    {
        WriteHardware(phys, data, 4);
        return;
    }
    /* Cache control */
    if (phys == 0x1FFE0130)
    {
        cache_ctrl = data;
        return;
    }
    /* Ignore writes to BIOS ROM */
}

/* ---- LWL/LWR/SWL/SWR helpers (little-endian PSX) ---- */

uint32_t Helper_LWL(uint32_t addr, uint32_t cur_rt)
{
    uint32_t aligned = addr & ~3;
    uint32_t word = ReadWord(aligned);
    switch (addr & 3)
    {
    case 0:
        return (word & 0x000000FF) << 24 | (cur_rt & 0x00FFFFFF);
    case 1:
        return (word & 0x0000FFFF) << 16 | (cur_rt & 0x0000FFFF);
    case 2:
        return (word & 0x00FFFFFF) << 8 | (cur_rt & 0x000000FF);
    case 3:
        return word;
    }
    return cur_rt; /* unreachable */
}

uint32_t Helper_LWR(uint32_t addr, uint32_t cur_rt)
{
    uint32_t aligned = addr & ~3;
    uint32_t word = ReadWord(aligned);
    switch (addr & 3)
    {
    case 0:
        return word;
    case 1:
        return (word >> 8) | (cur_rt & 0xFF000000);
    case 2:
        return (word >> 16) | (cur_rt & 0xFFFF0000);
    case 3:
        return (word >> 24) | (cur_rt & 0xFFFFFF00);
    }
    return cur_rt; /* unreachable */
}

void Helper_SWL(uint32_t addr, uint32_t rt_val)
{
    uint32_t aligned = addr & ~3;
    uint32_t word = ReadWord(aligned);
    uint32_t result;
    switch (addr & 3)
    {
    case 0:
        result = (word & 0xFFFFFF00) | (rt_val >> 24);
        break;
    case 1:
        result = (word & 0xFFFF0000) | (rt_val >> 16);
        break;
    case 2:
        result = (word & 0xFF000000) | (rt_val >> 8);
        break;
    case 3:
        result = rt_val;
        break;
    default:
        return;
    }
    WriteWord(aligned, result);
}

void Helper_SWR(uint32_t addr, uint32_t rt_val)
{
    uint32_t aligned = addr & ~3;
    uint32_t word = ReadWord(aligned);
    uint32_t result;
    switch (addr & 3)
    {
    case 0:
        result = rt_val;
        break;
    case 1:
        result = (word & 0x000000FF) | (rt_val << 8);
        break;
    case 2:
        result = (word & 0x0000FFFF) | (rt_val << 16);
        break;
    case 3:
        result = (word & 0x00FFFFFF) | (rt_val << 24);
        break;
    default:
        return;
    }
    WriteWord(aligned, result);
}

/* ---- DIV/DIVU helpers (correct edge cases) ---- */

void Helper_DIV(int32_t rs, int32_t rt, uint32_t *lo_out, uint32_t *hi_out)
{
    if (rt == 0)
    {
        /* Division by zero: R3000A behavior */
        *hi_out = (uint32_t)rs;
        *lo_out = (rs >= 0) ? (uint32_t)(int32_t)-1 : (uint32_t)1;
    }
    else if ((uint32_t)rs == 0x80000000 && rt == -1)
    {
        /* MIN_INT / -1: overflow, R3000A gives LO=MIN_INT, HI=0 */
        *lo_out = 0x80000000;
        *hi_out = 0;
    }
    else
    {
        *lo_out = (uint32_t)(rs / rt);
        *hi_out = (uint32_t)(rs % rt);
    }
}

void Helper_DIVU(uint32_t rs, uint32_t rt, uint32_t *lo_out, uint32_t *hi_out)
{
    if (rt == 0)
    {
        /* Division by zero: R3000A behavior */
        *hi_out = rs;
        *lo_out = 0xFFFFFFFF;
    }
    else
    {
        *lo_out = rs / rt;
        *hi_out = rs % rt;
    }
}
