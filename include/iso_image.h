#ifndef ISO_IMAGE_H
#define ISO_IMAGE_H

#include <stdint.h>

/*
 * ISO Image Backend
 *
 * Provides sector-level read access to ISO 9660 disc images.
 * Supports standard 2048-byte sector ISO files and 2352-byte raw/BIN images.
 */

/* Sector sizes */
#define ISO_SECTOR_SIZE     2048
#define RAW_SECTOR_SIZE     2352
#define RAW_DATA_OFFSET     24   /* Sync(12) + Header(4) + Subheader(8) for Mode 2 Form 1 */
#define RAW_MODE1_OFFSET    16   /* Sync(12) + Header(4) for Mode 1 */

/*
 * Open an ISO image file.
 * Detects format (2048 vs 2352 byte sectors) automatically.
 * Returns 0 on success, < 0 on error.
 */
int ISO_Open(const char *path);

/*
 * Open a disc image from a CUE sheet.
 * Parses the CUE file to find the BIN filename, resolves its path
 * relative to the CUE file, and opens it via ISO_Open().
 * Returns 0 on success, < 0 on error.
 */
int ISO_OpenCue(const char *cue_path);

/*
 * Read a single 2048-byte sector of user data at the given LBA.
 * Returns 0 on success, -1 on error or out-of-range.
 */
int ISO_ReadSector(uint32_t lba, uint8_t *buf);

/*
 * Returns 1 if an ISO image is currently loaded/mounted.
 */
int ISO_IsLoaded(void);

/*
 * Returns the total number of sectors in the mounted ISO.
 */
uint32_t ISO_GetSectorCount(void);

/*
 * Close the ISO image and release resources.
 */
void ISO_Close(void);

#endif /* ISO_IMAGE_H */
