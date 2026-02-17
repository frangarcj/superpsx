#include <stdint.h>
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
#include "iso_image.h"
#include "iso_fs.h"

#include <string.h>
#include <limits.h>

/* Default executable filename; store in an internal buffer to avoid
 * depending on `argv` pointer lifetime or external mutation. */
#ifndef PSX_EXE_PATH_MAX
#define PSX_EXE_PATH_MAX 512
#endif
static char psx_exe_filename_buf[PSX_EXE_PATH_MAX] = "test.exe";
const char *psx_exe_filename = psx_exe_filename_buf;
int psx_boot_mode = BOOT_MODE_EXE;

/* Check if a filename has a disc image extension */
static int has_disc_extension(const char *filename)
{
    size_t len = strlen(filename);
    if (len < 4)
        return 0;
    const char *ext = filename + len - 4;
    return (strcasecmp(ext, ".iso") == 0 ||
            strcasecmp(ext, ".bin") == 0 ||
            strcasecmp(ext, ".cue") == 0);
}

static int has_cue_extension(const char *filename)
{
    size_t len = strlen(filename);
    if (len < 4)
        return 0;
    return strcasecmp(filename + len - 4, ".cue") == 0;
}

static void reset_IOP()
{
    SifInitRpc(0);
    /* Comment this line if you don't wanna debug the output */
    while (!SifIopReset(NULL, 0))
    {
    }
    while (!SifIopSync())
    {
    }
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
    printf("SuperPSX v0.2 - Native Dynarec\n");
    printf("Initializing SuperPSX... with %d arguments\n", argc);
    for (int i = 0; i < argc; i++)
    {
        printf("  argv[%d]: %s\n", i, argv[i]);
    }
    if (argc > 1)
    {
        /* argv[1] = host PWD, argv[2] = PSX exe filename */
        /* chdir to the provided PWD if possible */

        if (strcasecmp(argv[0], "host") && chdir(argv[1]) != 0)
        {
            printf("WARNING: Failed to chdir to %s\n", argv[1]);
        }

        /* Copy and sanitize filename into internal buffer */
        size_t len = strnlen(argv[1], PSX_EXE_PATH_MAX - 1);
        if (len == PSX_EXE_PATH_MAX - 1)
        {
            /* truncated */
            printf("WARNING: PSX exe filename too long, truncated.\n");
        }
        strncpy(psx_exe_filename_buf, argv[1], PSX_EXE_PATH_MAX - 1);
        psx_exe_filename_buf[PSX_EXE_PATH_MAX - 1] = '\0';

        /* Remove surrounding quotes if present */
        if (psx_exe_filename_buf[0] == '"')
        {
            size_t i;
            for (i = 0; i + 1 < PSX_EXE_PATH_MAX && psx_exe_filename_buf[i + 1] != '\0'; ++i)
                psx_exe_filename_buf[i] = psx_exe_filename_buf[i + 1];
            psx_exe_filename_buf[i] = '\0';
            /* remove trailing quote if left */
            len = strlen(psx_exe_filename_buf);
            if (len > 0 && psx_exe_filename_buf[len - 1] == '"')
                psx_exe_filename_buf[len - 1] = '\0';
        }

        psx_exe_filename = psx_exe_filename_buf;
        printf("Using PSX exe: %s (cwd set to %s)\n", psx_exe_filename, argv[0]);
    }

    Init_SuperPSX();

    scr_printf("SuperPSX finished.\n");

    deinit_drivers();

    SleepThread(); // Halt the main thread (or exit cleanly if desired)
    return 0;
}

void Init_SuperPSX(void)
{
    printf("=== SuperPSX Initializing ===\n");
    fflush(stdout);

    Init_Graphics();

    Init_Memory();
    Init_Interrupts();
    CDROM_Init();

    /* Detect ISO disc image and mount it */
    if (psx_exe_filename && has_disc_extension(psx_exe_filename))
    {
        psx_boot_mode = BOOT_MODE_ISO;
        printf("Disc image detected: %s\n", psx_exe_filename);

        int open_result;
        if (has_cue_extension(psx_exe_filename))
            open_result = ISO_OpenCue(psx_exe_filename);
        else
            open_result = ISO_Open(psx_exe_filename);

        if (open_result < 0)
        {
            printf("ERROR: Failed to open disc image: %s\n", psx_exe_filename);
            scr_printf("Failed to open disc image. Halting.\n");
            while (1)
                ;
        }

        if (ISOFS_Init() < 0)
        {
            printf("ERROR: Failed to parse ISO 9660 filesystem\n");
            scr_printf("Failed to parse ISO. Halting.\n");
            while (1)
                ;
        }

        /* Report what we found */
        {
            char boot_path[256];
            if (ISOFS_ReadBootPath(boot_path, sizeof(boot_path)) == 0)
                printf("Boot executable from SYSTEM.CNF: %s\n", boot_path);
            else
                printf("WARNING: Could not parse SYSTEM.CNF boot path\n");
        }

        CDROM_InsertDisc();
        printf("ISO mounted, disc inserted. BIOS will boot from CD.\n");

        /* Clear the EXE filename so the BIOS shell hook won't intercept */
        psx_exe_filename_buf[0] = '\0';
    }
    else
    {
        psx_boot_mode = BOOT_MODE_EXE;
    }

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
