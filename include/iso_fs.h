#ifndef ISO_FS_H
#define ISO_FS_H

#include <stdint.h>

/*
 * ISO 9660 Filesystem Parser
 *
 * Parses the ISO 9660 filesystem on a mounted ISO image to locate files,
 * read SYSTEM.CNF, and extract the boot executable path for PSX discs.
 *
 * Requires iso_image.h to be initialized first (ISO_Open called).
 */

/*
 * Initialize the ISO 9660 filesystem parser.
 * Reads the Primary Volume Descriptor (PVD) at LBA 16 and caches
 * root directory information.
 * Returns 0 on success, < 0 on error.
 */
int ISOFS_Init(void);

/*
 * Find a file in the root directory by name.
 * Handles ISO 9660 filename conventions (uppercase, ";1" version suffix).
 * On success, returns 0 and fills lba_out and size_out.
 * Returns -1 if file not found.
 */
int ISOFS_FindFile(const char *name, uint32_t *lba_out, uint32_t *size_out);

/*
 * Read and parse SYSTEM.CNF from the disc root directory.
 * Extracts the boot executable path (e.g., "PSX.EXE" or "SLUS_012.34;1").
 * Writes the normalized filename (without "cdrom:\" prefix) into boot_path.
 * Returns 0 on success, < 0 on error.
 */
int ISOFS_ReadBootPath(char *boot_path, size_t max_len);

/*
 * Read a file's contents from the ISO given its LBA and size.
 * Reads up to buf_size bytes.
 * Returns the number of bytes actually read, or < 0 on error.
 */
int ISOFS_ReadFile(uint32_t file_lba, uint32_t file_size,
                   uint8_t *buf, uint32_t buf_size);

#endif /* ISO_FS_H */
