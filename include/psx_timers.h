#ifndef TIMERS_H
#define TIMERS_H

#include <stdint.h>

uint32_t Timers_Read(uint32_t addr);
void Timers_Write(uint32_t addr, uint32_t data);
void Timer_RefreshDividerCache(void);
void Timer_ScheduleAll(void);

#endif
