/**
 * init.c — Shared SuperPSX initialization (platform-agnostic)
 *
 * Contains Init_SuperPSX() which is called by all platform main_*.c files.
 */
#include <stdio.h>
#include <string.h>

#include "superpsx.h"
#include "config.h"
#include "gpu_backend.h"
#include "iso_image.h"
#include "iso_fs.h"
#include "osd.h"
#include "mdec.h"

/* Provided by the platform-specific main_*.c */
extern char psx_exe_filename_buf[];
extern const char *psx_exe_filename;
extern int psx_boot_mode;
extern const char **psx_host_args;
extern int psx_host_argc;
extern char disc_image_path[];

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

void Init_SuperPSX(void)
{
    printf("=== SuperPSX Initializing ===\n");
    fflush(stdout);

    GPU_Backend_Init();
    osd_boot_log("SuperPSX v0.2 - Native Dynarec");

    Init_Memory();
    Init_Interrupts();
    CDROM_Init();
    MDEC_Init();

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
                return;
            }

            if (ISOFS_Init() < 0)
            {
                osd_boot_log("ERROR: Failed to parse ISO");
                return;
            }

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

            /* Mount disc image alongside EXE if provided */
            if (disc_image_path[0] != '\0')
            {
                int open_result;
                if (has_cue_extension(disc_image_path))
                    open_result = ISO_OpenCue(disc_image_path);
                else
                    open_result = ISO_Open(disc_image_path);

                if (open_result >= 0)
                {
                    CDROM_InsertDisc();
                    osd_boot_log("Disc: %s (sideload)", disc_image_path);
                }
                else
                {
                    osd_boot_log("WARNING: Failed to open disc: %s",
                                 disc_image_path);
                }
            }
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
        return;
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
