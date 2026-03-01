/**
 * config.h — INI-style config file reader for SuperPSX
 *
 * load_config_file() reads "superpsx.ini" next to the ELF, populates
 * psx_config and (if a 'rom' key is present) psx_exe_filename.
 */
#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_FILENAME "superpsx.ini"
#define BIOS_PATH_DEFAULT "bios/SCPH1001.BIN"

typedef struct {
    char bios_path[512];      /* default: BIOS_PATH_DEFAULT */
    int  boot_bios_only;      /* 1 = boot to BIOS shell, no ROM required */
    int  audio_enabled;       /* default 1 */
    int  controllers_enabled; /* default 1 */
    int  region_pal;          /* 0 = NTSC (default), 1 = PAL */
    int  disable_audio;       /* 1 = skip SPU processing (profiling) */
    int  disable_gpu;         /* 1 = skip GS rendering (profiling) */
    int  frame_limit;         /* 1 = cap at 60fps NTSC / 50fps PAL (default 1) */
} PSXConfig;

extern PSXConfig psx_config;

/**
 * Load the config file and populate psx_config.
 * Returns 1 if the INI file was found and parsed (even with only 'boot=bios').
 * Returns 0 if the file was not found.
 */
int load_config_file(void);

#endif /* CONFIG_H */
