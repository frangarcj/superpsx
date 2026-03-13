/**
 * platform_ps2.c — PS2 platform implementation
 *
 * IOP initialization, filesystem drivers, cache management.
 */
#include "platform.h"

#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <ps2_filesystem_driver.h>

void Platform_Init(void)
{
    SifInitRpc(0);
    while (!SifIopReset(NULL, 0)) {}
    while (!SifIopSync()) {}
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    sbv_patch_fileio();

    init_only_boot_ps2_filesystem_driver();
}

void Platform_Halt(void)
{
    deinit_only_boot_ps2_filesystem_driver();
    SleepThread();
}

void Platform_FlushDCache(void *start, void *end)
{
    if (start == NULL)
        FlushCache(0); /* writeback entire D-cache */
    else
        SyncDCache(start, end);
}

void Platform_FlushICache(void)
{
    FlushCache(2); /* invalidate entire I-cache */
}

uint64_t Platform_GetCycles(void)
{
    /* PS2 COP0 Count register — stub for now */
    return 0;
}

void Platform_Sleep(uint32_t ms)
{
    (void)ms;
}
