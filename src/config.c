/**
 * config.c — INI-style config file reader for SuperPSX
 */
#include "config.h"
#include "superpsx.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Defined in main.c — internal buffer backing psx_exe_filename */
#ifndef PSX_EXE_PATH_MAX
#define PSX_EXE_PATH_MAX 512
#endif
extern char psx_exe_filename_buf[PSX_EXE_PATH_MAX];

#define CONFIG_LINE_MAX 1024

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
    FILE *fp = fopen(CONFIG_FILENAME, "r");
    if (!fp)
    {
        printf("CONFIG: No config file found (%s)\n", CONFIG_FILENAME);
        return 0;
    }

    printf("CONFIG: Reading %s\n", CONFIG_FILENAME);

    char line[CONFIG_LINE_MAX];
    int found_rom = 0;

    while (fgets(line, sizeof(line), fp))
    {
        char *trimmed = str_trim(line);

        /* Skip empty lines and comments */
        if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';')
            continue;

        /* Look for 'key = value' */
        char *eq = strchr(trimmed, '=');
        if (!eq)
            continue;

        *eq = '\0';
        char *key = str_trim(trimmed);
        char *val = str_trim(eq + 1);

        if (strcasecmp(key, "rom") == 0 && val[0] != '\0')
        {
            strncpy(psx_exe_filename_buf, val, PSX_EXE_PATH_MAX - 1);
            psx_exe_filename_buf[PSX_EXE_PATH_MAX - 1] = '\0';
            psx_exe_filename = psx_exe_filename_buf;
            found_rom = 1;
            printf("CONFIG: rom = %s\n", psx_exe_filename);
        }
    }

    fclose(fp);

    if (!found_rom)
        printf("CONFIG: No 'rom' key found in %s\n", CONFIG_FILENAME);

    return found_rom;
}
