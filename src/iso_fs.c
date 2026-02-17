/*
 * SuperPSX – ISO 9660 Filesystem Parser
 *
 * Parses ISO 9660 filesystems to locate files on PSX disc images.
 * Used to find SYSTEM.CNF and the boot executable.
 *
 * Reference: ECMA-119 (ISO 9660)
 *   - Primary Volume Descriptor at LBA 16
 *   - Root directory record at PVD offset 156
 *   - Directory records are variable-length with file identifiers
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include "iso_fs.h"
#include "iso_image.h"

#define LOG_TAG "ISOFS"

#ifdef ENABLE_DEBUG_LOG
#define DLOG(fmt, ...) printf("[" LOG_TAG "] " fmt, ##__VA_ARGS__)
#else
#define DLOG(...) \
    do            \
    {             \
    } while (0)
#endif

/* ---- Internal state ---- */
static struct
{
    uint32_t root_dir_lba;  /* LBA of root directory extent */
    uint32_t root_dir_size; /* Size of root directory in bytes */
    int initialized;        /* 1 if PVD has been parsed */
} isofs_state;

/* ---- ISO 9660 helpers ---- */

/* Read a little-endian 32-bit value from a buffer (ISO 9660 uses both-endian pairs) */
static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Case-insensitive comparison that also ignores trailing ";1" version suffix */
static int iso_name_match(const char *iso_name, int iso_name_len, const char *target)
{
    int target_len = strlen(target);

    /* Strip ";1" version suffix from ISO name if present */
    int cmp_len = iso_name_len;
    if (cmp_len >= 2 && iso_name[cmp_len - 1] == '1' && iso_name[cmp_len - 2] == ';')
        cmp_len -= 2;

    /* Strip ";1" from target too if present */
    int tgt_len = target_len;
    if (tgt_len >= 2 && target[tgt_len - 1] == '1' && target[tgt_len - 2] == ';')
        tgt_len -= 2;

    /* Also try stripping trailing '.' from ISO name (some discs have "FILE." for "FILE") */
    if (cmp_len > 0 && iso_name[cmp_len - 1] == '.')
        cmp_len--;

    if (cmp_len != tgt_len)
        return 0;

    for (int i = 0; i < cmp_len; i++)
    {
        if (toupper((unsigned char)iso_name[i]) != toupper((unsigned char)target[i]))
            return 0;
    }

    return 1; /* Match */
}

/* ---- Public API ---- */

int ISOFS_Init(void)
{
    uint8_t sector[ISO_SECTOR_SIZE];

    memset(&isofs_state, 0, sizeof(isofs_state));

    if (!ISO_IsLoaded())
    {
        printf("[ISOFS] ERROR: No ISO image mounted\n");
        return -1;
    }

    /* Read Primary Volume Descriptor at LBA 16 */
    if (ISO_ReadSector(16, sector) < 0)
    {
        printf("[ISOFS] ERROR: Failed to read PVD at LBA 16\n");
        return -2;
    }

    /* Validate PVD */
    if (sector[0] != 0x01)
    {
        printf("[ISOFS] ERROR: PVD type byte is 0x%02X, expected 0x01\n", sector[0]);
        return -3;
    }

    if (memcmp(&sector[1], "CD001", 5) != 0)
    {
        printf("[ISOFS] ERROR: PVD signature mismatch (expected 'CD001')\n");
        return -3;
    }

    printf("[ISOFS] Primary Volume Descriptor found at LBA 16\n");

    /* Volume identifier (bytes 40-71, 32 chars, padded with spaces) */
    char vol_id[33];
    memcpy(vol_id, &sector[40], 32);
    vol_id[32] = '\0';
    /* Trim trailing spaces */
    for (int i = 31; i >= 0 && vol_id[i] == ' '; i--)
        vol_id[i] = '\0';
    printf("[ISOFS] Volume ID: \"%s\"\n", vol_id);

    /* Root directory record starts at PVD offset 156, length 34 bytes */
    const uint8_t *root_record = &sector[156];

    /* Extract root directory extent LBA (offset 2 within record, little-endian) */
    isofs_state.root_dir_lba = read_le32(&root_record[2]);

    /* Extract root directory data length (offset 10, little-endian) */
    isofs_state.root_dir_size = read_le32(&root_record[10]);

    printf("[ISOFS] Root directory: LBA %" PRIu32 ", size %" PRIu32 " bytes\n",
           isofs_state.root_dir_lba, isofs_state.root_dir_size);

    isofs_state.initialized = 1;
    return 0;
}

int ISOFS_FindFile(const char *name, uint32_t *lba_out, uint32_t *size_out)
{
    uint8_t sector[ISO_SECTOR_SIZE];

    if (!isofs_state.initialized)
    {
        printf("[ISOFS] ERROR: Filesystem not initialized\n");
        return -1;
    }

    uint32_t dir_lba = isofs_state.root_dir_lba;
    uint32_t remaining = isofs_state.root_dir_size;
    uint32_t sectors = (remaining + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;

    DLOG("Searching for file: \"%s\" in root dir (LBA %" PRIu32 ", %" PRIu32 " sectors)\n",
         name, dir_lba, sectors);

    for (uint32_t s = 0; s < sectors; s++)
    {
        if (ISO_ReadSector(dir_lba + s, sector) < 0)
        {
            DLOG("Failed to read directory sector at LBA %" PRIu32 "\n", dir_lba + s);
            return -1;
        }

        uint32_t pos = 0;
        while (pos < ISO_SECTOR_SIZE)
        {
            uint8_t record_len = sector[pos];

            /* Record length 0 = padding / end of entries in this sector */
            if (record_len == 0)
                break;

            /* Sanity check */
            if (record_len < 33 || pos + record_len > ISO_SECTOR_SIZE)
                break;

            uint8_t name_len = sector[pos + 32];
            const char *file_name = (const char *)&sector[pos + 33];
            uint8_t file_flags = sector[pos + 25];

            /* Skip directories (bit 1 set) */
            if (!(file_flags & 0x02))
            {
                if (iso_name_match(file_name, name_len, name))
                {
                    *lba_out = read_le32(&sector[pos + 2]);
                    *size_out = read_le32(&sector[pos + 10]);
                    DLOG("Found \"%s\" at LBA %" PRIu32 ", size %" PRIu32 "\n", name, *lba_out, *size_out);
                    return 0;
                }
            }

            pos += record_len;
        }
    }

    DLOG("File \"%s\" not found in root directory\n", name);
    return -1;
}

int ISOFS_ReadBootPath(char *boot_path, size_t max_len)
{
    uint32_t cnf_lba, cnf_size;

    /* Find SYSTEM.CNF */
    if (ISOFS_FindFile("SYSTEM.CNF", &cnf_lba, &cnf_size) < 0)
    {
        printf("[ISOFS] ERROR: SYSTEM.CNF not found on disc\n");
        return -1;
    }

    /* Read the file (SYSTEM.CNF is typically < 256 bytes) */
    if (cnf_size > 2048)
        cnf_size = 2048; /* cap for safety */

    uint8_t cnf_buf[2048];
    int bytes_read = ISOFS_ReadFile(cnf_lba, cnf_size, cnf_buf, sizeof(cnf_buf));
    if (bytes_read < 0)
    {
        printf("[ISOFS] ERROR: Failed to read SYSTEM.CNF\n");
        return -2;
    }

    /* Null-terminate for string parsing */
    if ((uint32_t)bytes_read >= sizeof(cnf_buf))
        bytes_read = sizeof(cnf_buf) - 1;
    cnf_buf[bytes_read] = '\0';

    printf("[ISOFS] SYSTEM.CNF contents:\n%s\n", cnf_buf);

    /*
     * Parse SYSTEM.CNF looking for the BOOT line.
     * Formats seen in the wild:
     *   BOOT = cdrom:\PSX.EXE;1
     *   BOOT=cdrom:\SLUS_012.34;1
     *   BOOT = cdrom:\DIR\GAME.EXE;1
     *   BOOT = cdrom:PSX.EXE;1
     *   BOOT = cdrom1:\SCPS_123.45;1   (PS2 PSX discs)
     */
    const char *p = (const char *)cnf_buf;
    boot_path[0] = '\0';

    while (*p)
    {
        /* Skip whitespace and newlines */
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            p++;

        /* Check for "BOOT" keyword */
        if (strncasecmp(p, "BOOT", 4) == 0)
        {
            p += 4;
            /* Skip optional whitespace and '=' */
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '=')
                p++;
            while (*p == ' ' || *p == '\t')
                p++;

            /* Now p points to the path value, e.g., "cdrom:\PSX.EXE;1" */
            /* Skip the "cdrom:" or "cdrom1:" prefix */
            if (strncasecmp(p, "cdrom", 5) == 0)
            {
                p += 5;
                /* Skip optional digit after "cdrom" (e.g., "cdrom1") */
                if (*p >= '0' && *p <= '9')
                    p++;
                /* Skip ":" */
                if (*p == ':')
                    p++;
                /* Skip leading backslash or forward slash */
                if (*p == '\\' || *p == '/')
                    p++;
            }

            /* Copy the filename until end of line or semicolon delimiter */
            size_t i = 0;
            while (*p && *p != '\r' && *p != '\n' && i < max_len - 1)
            {
                /* Normalize backslash to forward slash */
                if (*p == '\\')
                    boot_path[i++] = '/';
                else
                    boot_path[i++] = *p;
                p++;
            }
            boot_path[i] = '\0';

            /* Trim trailing whitespace */
            while (i > 0 && (boot_path[i - 1] == ' ' || boot_path[i - 1] == '\t'))
            {
                boot_path[--i] = '\0';
            }

            printf("[ISOFS] Boot executable: \"%s\"\n", boot_path);
            return 0;
        }

        /* Skip to next line */
        while (*p && *p != '\n')
            p++;
    }

    printf("[ISOFS] ERROR: No BOOT line found in SYSTEM.CNF\n");
    return -3;
}

int ISOFS_ReadFile(uint32_t file_lba, uint32_t file_size,
                   uint8_t *buf, uint32_t buf_size)
{
    if (!isofs_state.initialized)
        return -1;

    uint32_t bytes_to_read = file_size;
    if (bytes_to_read > buf_size)
        bytes_to_read = buf_size;

    uint32_t sectors = (bytes_to_read + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    uint32_t total_read = 0;

    for (uint32_t s = 0; s < sectors; s++)
    {
        uint8_t sector_buf[ISO_SECTOR_SIZE];

        if (ISO_ReadSector(file_lba + s, sector_buf) < 0)
        {
            DLOG("ReadFile: Failed to read sector at LBA %" PRIu32 "\n", file_lba + s);
            return (total_read > 0) ? (int)total_read : -1;
        }

        uint32_t chunk = bytes_to_read - total_read;
        if (chunk > ISO_SECTOR_SIZE)
            chunk = ISO_SECTOR_SIZE;

        memcpy(buf + total_read, sector_buf, chunk);
        total_read += chunk;
    }

    DLOG("ReadFile: Read %" PRIu32 " bytes from LBA %" PRIu32 "\n", total_read, file_lba);
    return (int)total_read;
}
