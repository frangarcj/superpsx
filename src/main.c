#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <debug.h>
#include <unistd.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <stdio.h>

#include <ps2_filesystem_driver.h>
#include <ps2_audio_driver.h>


#include "superpsx.h"
#include "joystick.h"

static void reset_IOP()
{
    SifInitRpc(0);
	/* Comment this line if you don't wanna debug the output */
    while (!SifIopReset(NULL, 0)) {}
    while (!SifIopSync()) {}
}

static void prepare_IOP()
{
    reset_IOP();
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    sbv_patch_fileio();
}

static void init_drivers()
{
	init_only_boot_ps2_filesystem_driver();
	init_audio_driver();
    Joystick_Init();
}

static void deinit_drivers()
{
    Joystick_Shutdown();
	deinit_audio_driver();
	deinit_only_boot_ps2_filesystem_driver();
}

int main(int argc, char *argv[])
{
    prepare_IOP();
    init_drivers();

    init_scr();
    scr_printf("SuperPSX v0.2 - Native Dynarec\n");

    Init_SuperPSX();

    scr_printf("SuperPSX finished.\n");

    deinit_drivers();
    
    SleepThread(); // Halt the main thread (or exit cleanly if desired)
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

    if (Load_BIOS("bios/SCPH1001.BIN") < 0)
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
