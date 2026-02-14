#ifndef LOADER_H
#define LOADER_H

#include "superpsx.h"

/* PS-X EXE Header Structure */
typedef struct {
    char id[8];             /* "PS-X EXE" */
    u32 text_off;           /* Offset of text section in file */
    u32 data_off;           /* Offset of data section (unused usually) */
    u32 pc0;                /* Initial PC */
    u32 gp0;                /* Initial GP */
    u32 t_addr;             /* Destination address for text section */
    u32 t_size;             /* Size of text section */
    u32 d_addr;             /* Destination address for data section */
    u32 d_size;             /* Size of data section */
    u32 b_addr;             /* Destination address for bss section */
    u32 b_size;             /* Size of bss section */
    u32 s_addr;             /* Initial Stack Pointer base */
    u32 s_size;             /* Stack size */
    u32 sp0;                /* Initial SP (s_addr + s_size) */
    u32 fp0;                /* Initial FP/SP */
    u32 gp_off;             /* GP offset */
    u32 s_off;              /* Stack offset */
    char ascii_id[64];      /* ASCII ID string (e.g. "Sony Computer Entertainment Inc.") */
} PSEXE_Header;

/*
 * Load a PSX executable file into RAM and set up CPU registers.
 * Returns 0 on success, < 0 on error.
 */
int Load_PSX_EXE(const char *filename, R3000CPU *cpu);

#endif /* LOADER_H */
