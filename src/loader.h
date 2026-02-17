#ifndef LOADER_H
#define LOADER_H

#include "superpsx.h"

/* PS-X EXE Header Structure */
typedef struct
{
    char id[8];        /* "PS-X EXE" */
    uint32_t text_off; /* Offset of text section in file */
    uint32_t data_off; /* Offset of data section (unused usually) */
    uint32_t pc0;      /* Initial PC */
    uint32_t gp0;      /* Initial GP */
    uint32_t t_addr;   /* Destination address for text section */
    uint32_t t_size;   /* Size of text section */
    uint32_t d_addr;   /* Destination address for data section */
    uint32_t d_size;   /* Size of data section */
    uint32_t b_addr;   /* Destination address for bss section */
    uint32_t b_size;   /* Size of bss section */
    uint32_t s_addr;   /* Initial Stack Pointer base */
    uint32_t s_size;   /* Stack size */
    uint32_t sp0;      /* Initial SP (s_addr + s_size) */
    uint32_t fp0;      /* Initial FP/SP */
    uint32_t gp_off;   /* GP offset */
    uint32_t s_off;    /* Stack offset */
    char ascii_id[64]; /* ASCII ID string (e.g. "Sony Computer Entertainment Inc.") */
} PSEXE_Header;

/*
 * Load a PSX executable file into RAM and set up CPU registers.
 * Returns 0 on success, < 0 on error.
 */
int Load_PSX_EXE(const char *filename, R3000CPU *cpu);

/*
 * Load the boot executable from a mounted ISO image.
 * Reads SYSTEM.CNF to find the boot path, then reads the EXE from the ISO.
 * Returns 0 on success, < 0 on error.
 */
int Load_PSX_EXE_FromISO(R3000CPU *cpu);

#endif /* LOADER_H */
