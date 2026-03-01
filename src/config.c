/**
 * config.c — INI-style config file reader for SuperPSX
 */
#include "config.h"
#include "superpsx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>

/* Defined in main.c — internal buffer backing psx_exe_filename */
#ifndef PSX_EXE_PATH_MAX
#define PSX_EXE_PATH_MAX 512
#endif
extern char psx_exe_filename_buf[PSX_EXE_PATH_MAX];

#define CONFIG_LINE_MAX 1024

PSXConfig psx_config;

/* Trim leading and trailing whitespace in-place; return pointer into buf. */
static char *str_trim(char *buf)
{
    while (*buf == ' ' || *buf == '\t')
        ++buf;
    size_t l = strlen(buf);
    while (l > 0 && (buf[l - 1] == ' ' || buf[l - 1] == '\t' ||
                     buf[l - 1] == '\r' || buf[l - 1] == '\n'))
        buf[--l] = '\0';
    return buf;
}

int load_config_file(void)
{
    /* Apply defaults */
    psx_config.audio_enabled       = 1;
    psx_config.controllers_enabled = 1;
    psx_config.region_pal          = 0;
    psx_config.boot_bios_only      = 0;
    psx_config.disable_audio       = 0;
    psx_config.disable_gpu         = 0;
    psx_config.frame_limit         = 1;
    psx_config.gte_vu0             = 1;
    strncpy(psx_config.bios_path, BIOS_PATH_DEFAULT, sizeof(psx_config.bios_path) - 1);
    psx_config.bios_path[sizeof(psx_config.bios_path) - 1] = '\0';

    int fd = open(CONFIG_FILENAME, O_RDONLY);
    if (fd < 0)
    {
        printf("CONFIG: No config file found (%s)\n", CONFIG_FILENAME);
        return 0;
    }

    printf("CONFIG: Reading %s\n", CONFIG_FILENAME);

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0)
    {
        close(fd);
        return 0;
    }

    /* Use a stack-allocated buffer to avoid dynamic allocation */
    char buf[8192];
    ssize_t rr = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (rr <= 0)
    {
        return 0;
    }
    if ((size_t)rr >= sizeof(buf) - 1)
    {
        printf("CONFIG: Warning: config file truncated to %zu bytes\n", sizeof(buf) - 1);
        rr = sizeof(buf) - 1;
    }
    buf[rr] = '\0';

    char *line = buf;
    while (line && *line)
    {
        char *nl = strchr(line, '\n');
        char *next = nl ? nl + 1 : NULL;
        if (nl)
            *nl = '\0';

        char *trimmed = str_trim(line);

        /* Skip empty lines and comments */
        if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';')
        {
            line = next;
            continue;
        }

        /* Look for 'key = value' */
        char *eq = strchr(trimmed, '=');
        if (!eq)
        {
            line = next;
            continue;
        }

        *eq = '\0';
        char *key = str_trim(trimmed);
        char *val = str_trim(eq + 1);

        if (strcasecmp(key, "rom") == 0 && val[0] != '\0')
        {
            strncpy(psx_exe_filename_buf, val, PSX_EXE_PATH_MAX - 1);
            psx_exe_filename_buf[PSX_EXE_PATH_MAX - 1] = '\0';
            psx_exe_filename = psx_exe_filename_buf;
            printf("CONFIG: rom = %s\n", psx_exe_filename);
        }
        else if (strcasecmp(key, "boot") == 0)
        {
            if (strcasecmp(val, "bios") == 0)
                psx_config.boot_bios_only = 1;
            else
                psx_config.boot_bios_only = 0;
            printf("CONFIG: boot = %s\n", val);
        }
        else if (strcasecmp(key, "bios") == 0 && val[0] != '\0')
        {
            strncpy(psx_config.bios_path, val, sizeof(psx_config.bios_path) - 1);
            psx_config.bios_path[sizeof(psx_config.bios_path) - 1] = '\0';
            printf("CONFIG: bios = %s\n", psx_config.bios_path);
        }
        else if (strcasecmp(key, "audio") == 0)
        {
            psx_config.audio_enabled = (strcasecmp(val, "disabled") != 0);
            printf("CONFIG: audio = %s\n", psx_config.audio_enabled ? "enabled" : "disabled");
        }
        else if (strcasecmp(key, "controllers") == 0)
        {
            psx_config.controllers_enabled = (strcasecmp(val, "disabled") != 0);
            printf("CONFIG: controllers = %s\n", psx_config.controllers_enabled ? "enabled" : "disabled");
        }
        else if (strcasecmp(key, "region") == 0)
        {
            psx_config.region_pal = (strcasecmp(val, "pal") == 0);
            printf("CONFIG: region = %s\n", psx_config.region_pal ? "pal" : "ntsc");
        }
        else if (strcasecmp(key, "disable_audio") == 0)
        {
            psx_config.disable_audio = (atoi(val) != 0 || strcasecmp(val, "true") == 0);
            printf("CONFIG: disable_audio = %d\n", psx_config.disable_audio);
        }
        else if (strcasecmp(key, "disable_gpu") == 0)
        {
            psx_config.disable_gpu = (atoi(val) != 0 || strcasecmp(val, "true") == 0);
            printf("CONFIG: disable_gpu = %d\n", psx_config.disable_gpu);
        }
        else if (strcasecmp(key, "frame_limit") == 0)
        {
            psx_config.frame_limit = (atoi(val) != 0 && strcasecmp(val, "false") != 0);
            printf("CONFIG: frame_limit = %d\n", psx_config.frame_limit);
        }
        else if (strcasecmp(key, "gte_vu0") == 0)
        {
            psx_config.gte_vu0 = (atoi(val) != 0 && strcasecmp(val, "false") != 0);
            printf("CONFIG: gte_vu0 = %d\n", psx_config.gte_vu0);
        }
        line = next;
    }

    /* buf is stack-allocated; no free required */
    return 1;
}
