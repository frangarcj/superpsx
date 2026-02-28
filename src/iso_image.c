/*
 * SuperPSX – ISO Image Backend
 *
 * Opens and reads sectors from ISO 9660 disc images.
 * Supports both 2048-byte (standard ISO) and 2352-byte (raw/BIN) formats.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>
#include "superpsx.h"
#include "iso_image.h"

#define LOG_TAG "ISO"

/* ---- Internal state ---- */
static struct
{
    int fd;                 /* POSIX file descriptor */
    uint32_t total_sectors; /* Total number of user-data sectors */
    uint32_t sector_size;   /* 2048 or 2352 */
    uint32_t data_offset;   /* Offset to user data within each raw sector */
    int loaded;             /* 1 if image is mounted */
} iso_state;

/* ---- Public API ---- */

int ISO_Open(const char *path)
{
    if (iso_state.loaded)
        ISO_Close();

    memset(&iso_state, 0, sizeof(iso_state));
    iso_state.fd = -1;

    printf("[ISO] Opening image: %s\n", path);

    iso_state.fd = open(path, O_RDONLY);
    if (iso_state.fd < 0)
    {
        printf("[ISO] ERROR: Cannot open file: %s (%s)\n", path, strerror(errno));
        return -1;
    }

    /* Determine file size via fstat */
    struct stat st;
    if (fstat(iso_state.fd, &st) != 0 || st.st_size <= 0)
    {
        printf("[ISO] ERROR: Invalid file size for %s\n", path);
        close(iso_state.fd);
        iso_state.fd = -1;
        return -2;
    }
    long file_size = (long)st.st_size;

    /* Detect format: 2048-byte ISO vs 2352-byte raw/BIN */
    if ((file_size % RAW_SECTOR_SIZE) == 0)
    {
        /* Check for sync pattern at offset 0 to confirm raw format */
        uint8_t sync[12];
        if (lseek(iso_state.fd, 0, SEEK_SET) < 0 || read(iso_state.fd, sync, 12) != 12)
        {
            printf("[ISO] ERROR: Failed to read sync bytes\n");
            close(iso_state.fd);
            iso_state.fd = -1;
            return -3;
        }

        /* CD-ROM sync pattern: 00 FF FF FF FF FF FF FF FF FF FF 00 */
        static const uint8_t cd_sync[12] = {
            0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0x00};

        if (memcmp(sync, cd_sync, 12) == 0)
        {
            iso_state.sector_size = RAW_SECTOR_SIZE;
            /* Check mode byte at offset 15 to determine data offset */
            uint8_t mode_byte;
            if (lseek(iso_state.fd, 15, SEEK_SET) < 0 || read(iso_state.fd, &mode_byte, 1) != 1)
                mode_byte = 0;

            if (mode_byte == 2)
                iso_state.data_offset = RAW_DATA_OFFSET; /* Mode 2: sync(12)+header(4)+subheader(8) = 24 */
            else
                iso_state.data_offset = RAW_MODE1_OFFSET; /* Mode 1: sync(12)+header(4) = 16 */

            iso_state.total_sectors = (uint32_t)(file_size / RAW_SECTOR_SIZE);
            printf("[ISO] Detected raw/BIN format: %" PRIu32 " sectors (mode %d)\n",
                   iso_state.total_sectors, mode_byte);
        }
        else
        {
            /* No sync pattern, treat as 2048-byte ISO */
            iso_state.sector_size = ISO_SECTOR_SIZE;
            iso_state.data_offset = 0;
            iso_state.total_sectors = (uint32_t)(file_size / ISO_SECTOR_SIZE);
            printf("[ISO] Detected ISO format: %" PRIu32 " sectors\n", iso_state.total_sectors);
        }
    }
    else if ((file_size % ISO_SECTOR_SIZE) == 0)
    {
        iso_state.sector_size = ISO_SECTOR_SIZE;
        iso_state.data_offset = 0;
        iso_state.total_sectors = (uint32_t)(file_size / ISO_SECTOR_SIZE);
        printf("[ISO] Detected ISO format: %" PRIu32 " sectors\n", iso_state.total_sectors);
    }
    else
    {
        printf("[ISO] WARNING: File size %ld not aligned to sector size, "
               "assuming 2048-byte sectors\n",
               file_size);
        iso_state.sector_size = ISO_SECTOR_SIZE;
        iso_state.data_offset = 0;
        iso_state.total_sectors = (uint32_t)(file_size / ISO_SECTOR_SIZE);
        printf("[ISO] Detected ISO format: %" PRIu32 " sectors\n", iso_state.total_sectors);
    }

    iso_state.loaded = 1;
    printf("[ISO] Image mounted: %" PRIu32 " sectors, %" PRIu32 " bytes/sector\n",
           iso_state.total_sectors, iso_state.sector_size);
    return 0;
}

int ISO_ReadSector(uint32_t lba, uint8_t *buf)
{
    if (!iso_state.loaded || iso_state.fd < 0)
        return -1;

    if (lba >= iso_state.total_sectors)
    {
        DLOG("ReadSector: LBA %" PRIu32 " out of range (max %" PRIu32 ")\n",
             lba, iso_state.total_sectors);
        return -1;
    }

    /* Calculate file offset */
    off_t offset = (off_t)lba * iso_state.sector_size + iso_state.data_offset;
    if (lseek(iso_state.fd, offset, SEEK_SET) < 0)
        return -1;
    ssize_t got = read(iso_state.fd, buf, ISO_SECTOR_SIZE);
    if (got != ISO_SECTOR_SIZE)
    {
        DLOG("ReadSector: Short read at LBA %" PRIu32 ": got %zd bytes\n", lba, got);
        if (got > 0 && got < ISO_SECTOR_SIZE)
            memset(buf + got, 0, ISO_SECTOR_SIZE - got);
        return -1;
    }

    return 0;
}

/* ---- CUE sheet parser ---- */
int ISO_OpenCue(const char *cue_path)
{
    int cue_fd = open(cue_path, O_RDONLY);
    if (cue_fd < 0)
    {
        printf("[ISO] ERROR: Cannot open CUE file: %s (%s)\n", cue_path, strerror(errno));
        return -1;
    }

    printf("[ISO] Parsing CUE sheet: %s\n", cue_path);

    struct stat st;
    if (fstat(cue_fd, &st) != 0 || st.st_size <= 0)
    {
        close(cue_fd);
        printf("[ISO] ERROR: Empty or invalid CUE file: %s\n", cue_path);
        return -1;
    }

    /* Read CUE into a stack buffer (avoid dynamic allocation) */
    char cue_buf[8192];
    ssize_t r = read(cue_fd, cue_buf, sizeof(cue_buf) - 1);
    close(cue_fd);
    if (r <= 0)
    {
        printf("[ISO] ERROR: Failed to read CUE file: %s\n", cue_path);
        return -1;
    }
    if ((size_t)r >= sizeof(cue_buf) - 1)
    {
        printf("[ISO] WARNING: CUE file truncated to %zu bytes\n", sizeof(cue_buf) - 1);
        r = sizeof(cue_buf) - 1;
    }
    cue_buf[r] = '\0';

    char bin_filename[256] = {0};
    char *line = cue_buf;
    while (line && *line)
    {
        char *next = strchr(line, '\n');
        if (next)
            *next = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;

        if (strncasecmp(p, "FILE", 4) == 0 && (p[4] == ' ' || p[4] == '\t'))
        {
            p += 4;
            while (*p == ' ' || *p == '\t')
                p++;

            if (*p == '"')
            {
                p++;
                char *end = strchr(p, '"');
                if (end)
                {
                    size_t len = (size_t)(end - p);
                    if (len >= sizeof(bin_filename))
                        len = sizeof(bin_filename) - 1;
                    memcpy(bin_filename, p, len);
                    bin_filename[len] = '\0';
                }
            }
            else
            {
                char *end = p;
                while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')
                    end++;
                size_t len = (size_t)(end - p);
                if (len >= sizeof(bin_filename))
                    len = sizeof(bin_filename) - 1;
                memcpy(bin_filename, p, len);
                bin_filename[len] = '\0';
            }
            break;
        }

        if (!next)
            break;
        line = next + 1;
    }

    /* cue_buf is stack-allocated */

    if (bin_filename[0] == '\0')
    {
        printf("[ISO] ERROR: No FILE directive found in CUE sheet\n");
        return -2;
    }

    printf("[ISO] CUE references BIN file: %s\n", bin_filename);

    /* Resolve BIN path relative to the CUE file's directory */
    char bin_path[512];
    const char *last_sep = strrchr(cue_path, '/');
    if (last_sep)
    {
        size_t dir_len = (size_t)(last_sep - cue_path + 1);
        if (dir_len + strlen(bin_filename) >= sizeof(bin_path))
        {
            printf("[ISO] ERROR: BIN path too long\n");
            return -3;
        }
        memcpy(bin_path, cue_path, dir_len);
        strcpy(bin_path + dir_len, bin_filename);
    }
    else
    {
        /* CUE is in current directory */
        strncpy(bin_path, bin_filename, sizeof(bin_path) - 1);
        bin_path[sizeof(bin_path) - 1] = '\0';
    }

    printf("[ISO] Opening BIN file: %s\n", bin_path);
    return ISO_Open(bin_path);
}

int ISO_IsLoaded(void)
{
    return iso_state.loaded;
}

uint32_t ISO_GetSectorCount(void)
{
    return iso_state.total_sectors;
}

void ISO_Close(void)
{
    if (iso_state.fd >= 0)
    {
        close(iso_state.fd);
        iso_state.fd = -1;
    }
    iso_state.loaded = 0;
    iso_state.total_sectors = 0;
    DLOG("Image closed\n");
}
