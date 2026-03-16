/**
 * platform_psp.c — PSP platform initialization, cache management, timing
 */
#include "platform.h"
#include <pspkernel.h>
#include <psputils.h>
#include <psppower.h>

void Platform_Init(void) {
    /* Max clock: CPU 333MHz, bus 166MHz (default is 222/111) */
    scePowerSetClockFrequency(333, 333, 166);
}

void Platform_Halt(void) {
    sceKernelExitGame();
}

void Platform_FlushDCache(void *start, void *end) {
    if (start == NULL) {
        sceKernelDcacheWritebackInvalidateAll();
    } else {
        size_t size = (uintptr_t)end - (uintptr_t)start;
        sceKernelDcacheWritebackInvalidateRange(start, size);
    }
}

void Platform_FlushICache(void) {
    sceKernelIcacheInvalidateAll();
}

uint64_t Platform_GetCycles(void) {
    return (uint64_t)sceKernelGetSystemTimeLow();
}

void Platform_Sleep(uint32_t ms) {
    sceKernelDelayThread(ms * 1000);
}

/* PSP has no hardware TLB — provide stubs */
uint32_t psx_tlb_base = 0;
void Setup_PSX_TLB(void) {}
