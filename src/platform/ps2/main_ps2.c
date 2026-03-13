/**
 * main_ps2.c — PS2 entry point
 *
 * Handles IOP setup, command-line parsing, and calls Init_SuperPSX.
 */
#include <stdint.h>
#include <kernel.h>
#include <sifrpc.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "superpsx.h"
#include "config.h"
#include "joystick.h"
#include "spu.h"
#include "platform.h"
#include "mdec.h"

#ifndef PSX_EXE_PATH_MAX
#define PSX_EXE_PATH_MAX 512
#endif

char psx_exe_filename_buf[PSX_EXE_PATH_MAX] = "";
const char *psx_exe_filename = psx_exe_filename_buf;

/* Host-provided PSX argv */
const char **psx_host_args = NULL;
int psx_host_argc = 0;

/* Boot mode: 0 = EXE, 1 = disc */
int psx_boot_mode = 0;

/* Optional disc image path (second argument) */
char disc_image_path[PSX_EXE_PATH_MAX] = "";

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

int main(int argc, char *argv[])
{
    Platform_Init();

    /* Load config first */
    load_config_file();

    if (argc > 1)
    {
        if (strcasecmp(argv[0], "host") && chdir(argv[1]) != 0)
            printf("WARNING: Failed to chdir to %s\n", argv[1]);

        /* Copy and sanitize filename */
        size_t len = strnlen(argv[1], PSX_EXE_PATH_MAX - 1);
        if (len == PSX_EXE_PATH_MAX - 1)
            printf("WARNING: PSX exe filename too long, truncated.\n");
        strncpy(psx_exe_filename_buf, argv[1], PSX_EXE_PATH_MAX - 1);
        psx_exe_filename_buf[PSX_EXE_PATH_MAX - 1] = '\0';

        /* Remove surrounding quotes */
        if (psx_exe_filename_buf[0] == '"')
        {
            size_t i;
            for (i = 0; i + 1 < PSX_EXE_PATH_MAX &&
                 psx_exe_filename_buf[i + 1] != '\0'; ++i)
                psx_exe_filename_buf[i] = psx_exe_filename_buf[i + 1];
            psx_exe_filename_buf[i] = '\0';
            len = strlen(psx_exe_filename_buf);
            if (len > 0 && psx_exe_filename_buf[len - 1] == '"')
                psx_exe_filename_buf[len - 1] = '\0';
        }

        psx_exe_filename = psx_exe_filename_buf;
        psx_config.boot_bios_only = 0;
        printf("Using PSX exe from argv: %s (cwd set to %s)\n",
               psx_exe_filename, argv[0]);

        /* Check remaining args for disc image */
        if (argc > 2 && has_disc_extension(argv[2]))
        {
            strncpy(disc_image_path, argv[2], PSX_EXE_PATH_MAX - 1);
            disc_image_path[PSX_EXE_PATH_MAX - 1] = '\0';
            printf("Disc image from argv: %s\n", disc_image_path);
            if (argc > 3)
            {
                psx_host_argc = argc - 3;
                psx_host_args = (const char **)&argv[3];
            }
        }
        else if (argc > 2)
        {
            psx_host_argc = argc - 2;
            psx_host_args = (const char **)&argv[2];
        }
    }

    printf("SuperPSX v0.2 - Native Dynarec (PS2)\n");
    printf("Initializing SuperPSX... with %d arguments\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d]: %s\n", i, argv[i]);

    printf("CONFIG: boot=%s bios=%s audio=%s controllers=%s region=%s\n",
           psx_config.boot_bios_only ? "bios" : "rom",
           psx_config.bios_path,
           psx_config.audio_enabled ? "enabled" : "disabled",
           psx_config.controllers_enabled ? "enabled" : "disabled",
           psx_config.region_pal ? "pal" : "ntsc");

    if (!psx_config.boot_bios_only && psx_exe_filename_buf[0] == '\0')
    {
        printf("No ROM specified via argument or config file.\n");
        printf("Place a superpsx.ini next to the ELF with:\n"
               "  rom = path/to/game.cue\n  (or: boot = bios)\n");
        printf("Halting.\n");
        Platform_Halt();
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

    Platform_Halt();
    return 0;
}
