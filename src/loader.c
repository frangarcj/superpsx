#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include "loader.h"
#include "superpsx.h"

int Load_PSX_EXE(const char *filename, R3000CPU *cpu)
{
    printf("LOADER: Loading executable %s...\n", filename);

    int fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        printf("LOADER: Failed to open file %s\n", filename);
        return -1;
    }

    /* Read header */
    PSEXE_Header header;
    if (read(fd, &header, sizeof(header)) != sizeof(header))
    {
        printf("LOADER: Failed to read header\n");
        close(fd);
        return -2;
    }

    /* Validate header */
    if (strncmp(header.id, "PS-X EXE", 8) != 0)
    {
        printf("LOADER: Invalid PS-X EXE header: %.8s\n", header.id);
        close(fd);
        return -3;
    }

    printf("LOADER: Header info:\n");
    printf("  PC0: 0x%08X  GP0: 0x%08X\n", (unsigned)header.pc0, (unsigned)header.gp0);
    printf("  Text: 0x%08X (size 0x%X) -> File Off: 0x%X\n",
           (unsigned)header.t_addr, (unsigned)header.t_size, (unsigned)header.text_off);
    printf("  Stack: 0x%08X (size 0x%X)\n", (unsigned)header.s_addr, (unsigned)header.s_size);

    /* Seek to text section */
    /* text_off is usually 2048 (0x800) */
    lseek(fd, 2048, SEEK_SET);

    /* Load text section directly into PSX RAM */
    /* t_addr is usually in KSEG0 (0x80xxxxxx), convert to physical */
    uint32_t phys_addr = header.t_addr & 0x1FFFFFFF;
    if (phys_addr + header.t_size > PSX_RAM_SIZE)
    {
        printf("LOADER: Text section too large or out of bounds (phys 0x%08X, size 0x%X)\n",
               (unsigned)phys_addr, (unsigned)header.t_size);
        close(fd);
        return -4;
    }

    uint8_t *dest = psx_ram + phys_addr;
    int remaining = header.t_size;
    int total_read = 0;

    while (remaining > 0)
    {
        int chunk = (remaining > 65536) ? 65536 : remaining;
        int r = read(fd, dest + total_read, chunk);
        if (r <= 0)
            break;
        total_read += r;
        remaining -= r;
    }

    close(fd);

    if (total_read != header.t_size)
    {
        printf("LOADER: Warning: Read %d bytes, expected %d\n", total_read, (int)header.t_size);
    }
    else
    {
        printf("LOADER: Loaded %d bytes to RAM at 0x%08X\n", total_read, (unsigned)header.t_addr);
    }

    /* Set up CPU registers */
    cpu->pc = header.pc0;
    cpu->regs[28] = header.gp0; /* $gp */

    /* Set stack if provided */
    if (header.s_addr != 0)
    {
        cpu->regs[29] = header.s_addr + header.s_size; /* $sp */
        cpu->regs[30] = cpu->regs[29];                 /* $fp (frame pointer usually = sp at start) */
        printf("LOADER: SP set to 0x%08X\n", (unsigned)cpu->regs[29]);
    }
    else
    {
        /* Default stack? BIOS should have set one up, maybe just keep it? */
        printf("LOADER: Using existing SP: 0x%08X\n", (unsigned)cpu->regs[29]);
    }

    /* Typically args for main() are cleared */
    cpu->regs[4] = 0; /* a0 */
    cpu->regs[5] = 0; /* a1 */

    return 0;
}
