/*
 * SuperPSX – ISO Image Backend
 *
 * Opens and reads sectors from ISO 9660 disc images.
 * Supports both 2048-byte (standard ISO) and 2352-byte (raw/BIN) formats.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "iso_image.h"

#define LOG_TAG "ISO"

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
    FILE *fp;               /* Open file handle */
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

    printf("[ISO] Opening image: %s\n", path);

    iso_state.fp = fopen(path, "rb");
    if (!iso_state.fp)
    {
        printf("[ISO] ERROR: Cannot open file: %s\n", path);
        return -1;
    }

    /* Determine file size */
    fseek(iso_state.fp, 0, SEEK_END);
    long file_size = ftell(iso_state.fp);
    fseek(iso_state.fp, 0, SEEK_SET);

    if (file_size <= 0)
    {
        printf("[ISO] ERROR: Invalid file size: %ld\n", file_size);
        fclose(iso_state.fp);
        iso_state.fp = NULL;
        return -2;
    }

    /* Detect format: 2048-byte ISO vs 2352-byte raw/BIN */
    if ((file_size % RAW_SECTOR_SIZE) == 0)
    {
        /* Check for sync pattern at offset 0 to confirm raw format */
        uint8_t sync[12];
        fread(sync, 1, 12, iso_state.fp);
        fseek(iso_state.fp, 0, SEEK_SET);

        /* CD-ROM sync pattern: 00 FF FF FF FF FF FF FF FF FF FF 00 */
        static const uint8_t cd_sync[12] = {
            0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0x00};

        if (memcmp(sync, cd_sync, 12) == 0)
        {
            iso_state.sector_size = RAW_SECTOR_SIZE;
            /* Check mode byte at offset 15 to determine data offset */
            uint8_t mode_byte;
            fseek(iso_state.fp, 15, SEEK_SET);
            fread(&mode_byte, 1, 1, iso_state.fp);
            fseek(iso_state.fp, 0, SEEK_SET);

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
        printf("[ISO] Detected ISO format: %u sectors\n", iso_state.total_sectors);
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
    if (!iso_state.loaded || !iso_state.fp)
        return -1;

    if (lba >= iso_state.total_sectors)
    {
        DLOG("ReadSector: LBA %" PRIu32 " out of range (max %" PRIu32 ")\n",
             lba, iso_state.total_sectors);
        return -1;
    }

    /* Calculate file offset */
    long offset = (long)lba * iso_state.sector_size + iso_state.data_offset;

    if (fseek(iso_state.fp, offset, SEEK_SET) != 0)
    {
        DLOG("ReadSector: fseek failed for LBA %" PRIu32 " (offset %ld)\n", lba, offset);
        return -1;
    }

    size_t read = fread(buf, 1, ISO_SECTOR_SIZE, iso_state.fp);
    if (read != ISO_SECTOR_SIZE)
    {
        DLOG("ReadSector: Short read at LBA %u: got %zu bytes\n", lba, read);
        /* Zero-fill remaining bytes */
        if (read < ISO_SECTOR_SIZE)
            memset(buf + read, 0, ISO_SECTOR_SIZE - read);
        return -1;
    }

    return 0;
}

/* ---- CUE sheet parser ---- */
int ISO_OpenCue(const char *cue_path)
{
    FILE *cue = fopen(cue_path, "r");
    if (!cue)
    {
        printf("[ISO] ERROR: Cannot open CUE file: %s\n", cue_path);
        return -1;
    }

    printf("[ISO] Parsing CUE sheet: %s\n", cue_path);

    char bin_filename[256] = {0};
    char line[512];

    while (fgets(line, sizeof(line), cue))
    {
        /* Look for: FILE "filename.bin" BINARY */
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;

        if (strncasecmp(p, "FILE", 4) == 0 && (p[4] == ' ' || p[4] == '\t'))
        {
            p += 4;
            while (*p == ' ' || *p == '\t')
                p++;

            /* Extract filename (may be quoted) */
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
                /* Unquoted: take until space */
                char *end = p;
                while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')
                    end++;
                size_t len = (size_t)(end - p);
                if (len >= sizeof(bin_filename))
                    len = sizeof(bin_filename) - 1;
                memcpy(bin_filename, p, len);
                bin_filename[len] = '\0';
            }
            break; /* Use the first FILE directive */
        }
    }
    fclose(cue);

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
    if (iso_state.fp)
    {
        fclose(iso_state.fp);
        iso_state.fp = NULL;
    }
    iso_state.loaded = 0;
    iso_state.total_sectors = 0;
    DLOG("Image closed\n");
}
