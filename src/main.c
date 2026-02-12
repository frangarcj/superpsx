#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <debug.h>
#include <unistd.h>
#include <iopcontrol.h>
#include <stdio.h>

#include "superpsx.h"

int main(int argc, char *argv[]) {
    SifInitRpc(0);

    /* Reset IOP to clean state */
    while (!SifIopReset("", 0));
    while (!SifIopSync());

    SifInitRpc(0);

    init_scr();
    scr_printf("SuperPSX v0.2 - Native Dynarec\n");

    Init_SuperPSX();

    scr_printf("SuperPSX finished.\n");

    while (1); /* Halt */
    return 0;
}

void Init_SuperPSX(void) {
    printf("=== SuperPSX Initializing ===\n");
    fflush(stdout);

    Init_Graphics();
    Init_Memory();

    if (Load_BIOS("host:SCPH1001.BIN") < 0) {
        printf("ERROR: Failed to load BIOS!\n");
        scr_printf("Failed to load BIOS. Halting.\n");
        while (1);
    }

    Init_CPU();
    Init_Dynarec();

    printf("=== Starting Execution ===\n");
    fflush(stdout);
    scr_printf("Starting PSX BIOS execution...\n");

    Run_CPU();

    printf("=== Execution Ended ===\n");
    fflush(stdout);
}
