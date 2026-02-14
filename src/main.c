#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <debug.h>
#include <unistd.h>
#include <iopcontrol.h>
#include <stdio.h>

#include "superpsx.h"

int main(int argc, char *argv[])
{
    SifInitRpc(0);

    /* Reset IOP to clean state */
    while (!SifIopReset("", 0))
        ;
    while (!SifIopSync())
        ;

    SifInitRpc(0);

    init_scr();
    scr_printf("SuperPSX v0.2 - Native Dynarec\n");

    Init_SuperPSX();

    scr_printf("SuperPSX finished.\n");

    while (1)
        ; /* Halt */
    return 0;
}

void Test_Primitives(void)
{
    printf("=== Testing Primitives ===\n");

    // Test Triangle Flat Red
    // 0x20 = Tri Flat NoTex NoSemi, Color = 0x0000FF (BGR: BB=00, GG=00, RR=FF -> Red)
    GPU_WriteGP0(0x200000FF);
    GPU_WriteGP0(0x00640064); // X=100, Y=100
    GPU_WriteGP0(0x00960064); // X=150, Y=100
    GPU_WriteGP0(0x007D0096); // X=125, Y=150

    // Test Quad Flat Blue
    // 0x28 = Quad Flat NoTex NoSemi, Color = 0xFF0000 (BGR: BB=FF, GG=00, RR=00 -> Blue)
    GPU_WriteGP0(0x28FF0000);
    GPU_WriteGP0(0x00C80064); // X=200, Y=100
    GPU_WriteGP0(0x00FA0064); // X=250, Y=100
    GPU_WriteGP0(0x00FA0096); // X=250, Y=150
    GPU_WriteGP0(0x00C80096); // X=200, Y=150

    // Flush to ensure drawing
    GPU_Flush();

    printf("Primitives test done. Should see red triangle and blue quad.\n");
}

void Init_SuperPSX(void)
{
    printf("=== SuperPSX Initializing ===\n");
    fflush(stdout);

    Init_Graphics();

    // Test primitives
    Test_Primitives();

    Init_Memory();
    Init_Interrupts();
    CDROM_Init();

    if (Load_BIOS("host:SCPH1001.BIN") < 0)
    {
        printf("ERROR: Failed to load BIOS!\n");
        scr_printf("Failed to load BIOS. Halting.\n");
        while (1)
            ;
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
