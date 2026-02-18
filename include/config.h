/**
 * config.h — INI-style config file reader for SuperPSX
 *
 * When no ROM path is given on the command line, load_config_file()
 * reads "superpsx.ini" next to the ELF and populates psx_exe_filename.
 */
#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_FILENAME "superpsx.ini"

/**
 * Try to load the ROM path from the config file.
 * On success fills psx_exe_filename_buf / psx_exe_filename and returns 1.
 * Returns 0 if the file is missing or contains no 'rom' key.
 */
int load_config_file(void);

#endif /* CONFIG_H */
