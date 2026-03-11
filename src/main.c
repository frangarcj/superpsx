#include <stdint.h>
#include <kernel.h>
#include <sifrpc.h>
#include <unistd.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <stdio.h>

#include <ps2_filesystem_driver.h>
#include <ps2_audio_driver.h>

#include "superpsx.h"
#include "config.h"
#include "joystick.h"
#include "spu.h"
#include "iso_image.h"
#include "iso_fs.h"
#include "osd.h"

#include <string.h>
#include <limits.h>

/* Default executable filename; store in an internal buffer to avoid
 * depending on `argv` pointer lifetime or external mutation. */
#ifndef PSX_EXE_PATH_MAX
#define PSX_EXE_PATH_MAX 512
#endif
char psx_exe_filename_buf[PSX_EXE_PATH_MAX] = "";
const char *psx_exe_filename = psx_exe_filename_buf;
int psx_boot_mode = BOOT_MODE_EXE;

/* Host-provided PSX argv that will be written into the PSX scratchpad.
 * These are populated from `main` and consumed in `Init_SuperPSX`. */
static const char **psx_host_args = NULL;
static int psx_host_argc = 0;

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

int main(int argc, char *argv[])
{
    prepare_IOP();
    init_only_boot_ps2_filesystem_driver();

    /* Load config first; populates psx_config + maybe psx_exe_filename */
    load_config_file();

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
        psx_config.boot_bios_only = 0;
        printf("Using PSX exe from argv: %s (cwd set to %s)\n", psx_exe_filename, argv[0]);

        /* Capture any remaining command-line args (after the exe filename)
         * and expose them to the PSX executable via scratchpad. */
        if (argc > 2)
        {
            psx_host_argc = argc - 2;
            psx_host_args = (const char **)&argv[2];
        }
    }

    printf("SuperPSX v0.2 - Native Dynarec\n");
    printf("Initializing SuperPSX... with %d arguments\n", argc);
    for (int i = 0; i < argc; i++)
    {
        printf("  argv[%d]: %s\n", i, argv[i]);
    }

    /* Print config summary */
    printf("CONFIG: boot=%s bios=%s audio=%s controllers=%s region=%s\n",
           psx_config.boot_bios_only ? "bios" : "rom",
           psx_config.bios_path,
           psx_config.audio_enabled ? "enabled" : "disabled",
           psx_config.controllers_enabled ? "enabled" : "disabled",
           psx_config.region_pal ? "pal" : "ntsc");

    /* Validate: need a ROM unless booting to BIOS shell */
    if (!psx_config.boot_bios_only && psx_exe_filename_buf[0] == '\0')
    {
        printf("No ROM specified via argument or config file.\n");
        printf("No ROM specified.\nPlace a superpsx.ini next to the ELF with:\n  rom = path/to/game.cue\n  (or: boot = bios)\n");
        printf("Halting.\n");
        deinit_only_boot_ps2_filesystem_driver();
        SleepThread();
        return 1;
    }

    if (psx_config.audio_enabled)
        SPU_Init();

    if (psx_config.controllers_enabled)
        Joystick_Init();

    Init_SuperPSX();

    printf("SuperPSX finished.\n");

    if (psx_config.controllers_enabled)
        Joystick_Shutdown();

    if (psx_config.audio_enabled)
        SPU_Shutdown();

    deinit_only_boot_ps2_filesystem_driver();

    SleepThread(); // Halt the main thread (or exit cleanly if desired)
    return 0;
}

void Init_SuperPSX(void)
{
    printf("=== SuperPSX Initializing ===\n");
    fflush(stdout);

    Init_Graphics();
    osd_boot_log("SuperPSX v0.2 - Native Dynarec");

    Init_Memory();
    Init_Interrupts();
    CDROM_Init();

    /* If host provided PSX arguments, write them into scratchpad now. */
    if (psx_host_argc > 0 && psx_host_args)
    {
        PSX_SetArgs(psx_host_args, psx_host_argc);
        osd_boot_log("PSX args: %d written", psx_host_argc);
    }

    if (!psx_config.boot_bios_only)
    {
        /* Detect ISO disc image and mount it */
        if (psx_exe_filename && has_disc_extension(psx_exe_filename))
        {
            psx_boot_mode = BOOT_MODE_ISO;
            osd_boot_log("Disc: %s", psx_exe_filename);

            int open_result;
            if (has_cue_extension(psx_exe_filename))
                open_result = ISO_OpenCue(psx_exe_filename);
            else
                open_result = ISO_Open(psx_exe_filename);

            if (open_result < 0)
            {
                osd_boot_log("ERROR: Failed to open disc image");
                SleepThread();
            }

            if (ISOFS_Init() < 0)
            {
                osd_boot_log("ERROR: Failed to parse ISO");
                SleepThread();
            }

            /* Report what we found */
            {
                char boot_path[256];
                if (ISOFS_ReadBootPath(boot_path, sizeof(boot_path)) == 0)
                    osd_boot_log("Boot: %s", boot_path);
                else
                    osd_boot_log("WARNING: No SYSTEM.CNF boot path");
            }

            CDROM_InsertDisc();
            osd_boot_log("ISO mounted, disc inserted");

            /* Clear the EXE filename so the BIOS shell hook won't intercept */
            psx_exe_filename_buf[0] = '\0';
        }
        else
        {
            psx_boot_mode = BOOT_MODE_EXE;
        }
    }
    else
    {
        osd_boot_log("Boot mode: BIOS shell");
        psx_boot_mode = BOOT_MODE_EXE;
        psx_exe_filename_buf[0] = '\0';
    }

    if (Load_BIOS(psx_config.bios_path) < 0)
    {
        osd_boot_log("ERROR: BIOS load failed: %s", psx_config.bios_path);
        SleepThread();
    }
    osd_boot_log("BIOS loaded OK");

    Init_CPU();
    Init_Dynarec();

    osd_boot_log("Starting execution...");
    fflush(stdout);

    Run_CPU();

    printf("=== Execution Ended ===\n");
    fflush(stdout);
}
