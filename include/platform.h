#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

/* Platform initialization (IOP/drivers on PS2, kernel on PSP) */
void Platform_Init(void);

/* Platform cleanup and exit */
void Platform_Halt(void);

/* Cache management */
void Platform_FlushDCache(void *start, void *end);
void Platform_FlushICache(void);

/* Timing / Delay */
uint64_t Platform_GetCycles(void);
void Platform_Sleep(uint32_t ms);

#endif /* PLATFORM_H */
