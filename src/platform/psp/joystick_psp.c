/**
 * joystick_psp.c — PSP controller input via sceCtrl
 *
 * Maps PSP buttons to PSX digital controller format (0x41 ID + 2 button bytes).
 * PSP has a single controller with no multitap support.
 */
#include "joystick.h"
#include <pspctrl.h>
#include <string.h>

static int pad_initialized = 0;

void Joystick_Init(void) {
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    pad_initialized = 1;
}

void Joystick_Shutdown(void) {
    pad_initialized = 0;
}

/*
 * PSX digital controller response format:
 *   byte 0: controller ID (0x41 = digital pad)
 *   byte 1: buttons low  (active low: 0 = pressed)
 *            bit 0: SELECT, bit 1: L3, bit 2: R3, bit 3: START
 *            bit 4: UP, bit 5: RIGHT, bit 6: DOWN, bit 7: LEFT
 *   byte 2: buttons high (active low)
 *            bit 0: L2, bit 1: R2, bit 2: L1, bit 3: R1
 *            bit 4: TRIANGLE, bit 5: CIRCLE, bit 6: CROSS, bit 7: SQUARE
 */
static uint32_t read_psp_pad(void) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);

    uint8_t lo = 0xFF; /* All released (active low) */
    uint8_t hi = 0xFF;

    /* Map PSP buttons → PSX digital format */
    if (pad.Buttons & PSP_CTRL_SELECT)   lo &= ~(1 << 0);
    if (pad.Buttons & PSP_CTRL_START)    lo &= ~(1 << 3);
    if (pad.Buttons & PSP_CTRL_UP)       lo &= ~(1 << 4);
    if (pad.Buttons & PSP_CTRL_RIGHT)    lo &= ~(1 << 5);
    if (pad.Buttons & PSP_CTRL_DOWN)     lo &= ~(1 << 6);
    if (pad.Buttons & PSP_CTRL_LEFT)     lo &= ~(1 << 7);

    if (pad.Buttons & PSP_CTRL_LTRIGGER) hi &= ~(1 << 2); /* L1 */
    if (pad.Buttons & PSP_CTRL_RTRIGGER) hi &= ~(1 << 3); /* R1 */
    if (pad.Buttons & PSP_CTRL_TRIANGLE) hi &= ~(1 << 4);
    if (pad.Buttons & PSP_CTRL_CIRCLE)   hi &= ~(1 << 5);
    if (pad.Buttons & PSP_CTRL_CROSS)    hi &= ~(1 << 6);
    if (pad.Buttons & PSP_CTRL_SQUARE)   hi &= ~(1 << 7);

    return ((uint32_t)hi << 8) | lo;
}

uint32_t Joystick_Poll(void) {
    if (!pad_initialized) return 0xFFFF;
    return read_psp_pad();
}

uint32_t Joystick_PollPort(int port) {
    if (port != 0 || !pad_initialized) return 0xFFFF;
    return read_psp_pad();
}

int Joystick_HasMultitap(int port) {
    (void)port;
    return 0; /* PSP has no multitap */
}

int Joystick_IsConnected(int port, int slot) {
    (void)slot;
    return (port == 0) ? 1 : 0; /* Only port 0, slot 0 */
}

void Joystick_GetPSXDigitalResponse(int port, int slot, uint8_t response[3]) {
    (void)slot;
    if (port != 0 || !pad_initialized) {
        response[0] = 0xFF;
        response[1] = 0xFF;
        response[2] = 0xFF;
        return;
    }
    uint32_t buttons = read_psp_pad();
    response[0] = 0x41; /* Digital pad ID */
    response[1] = (uint8_t)(buttons & 0xFF);        /* Low byte */
    response[2] = (uint8_t)((buttons >> 8) & 0xFF);  /* High byte */
}
