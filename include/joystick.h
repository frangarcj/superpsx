#ifndef JOYSTICK_H
#define JOYSTICK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void Joystick_Init(void);
void Joystick_Shutdown(void);
uint32_t Joystick_Poll(void);
void Joystick_GetPSXDigitalResponse(uint8_t response[3]);

#ifdef __cplusplus
}
#endif

#endif // JOYSTICK_H
