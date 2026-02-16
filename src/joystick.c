#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <libpad.h>
#include <libmtap.h>
#include <ps2_joystick_driver.h>

#include "joystick.h"

#define PS2_MAX_PORT 2 /* each ps2 has 2 ports */
#define PS2_MAX_SLOT 4 /* maximum - 4 slots in one multitap */
#define MAX_CONTROLLERS (PS2_MAX_PORT * PS2_MAX_SLOT)

#define PAD_SELECT 0x0001
#define PAD_L3 0x0002
#define PAD_R3 0x0004
#define PAD_START 0x0008
#define PAD_UP 0x0010
#define PAD_RIGHT 0x0020
#define PAD_DOWN 0x0040
#define PAD_LEFT 0x0080
#define PAD_L2 0x0100
#define PAD_R2 0x0200
#define PAD_L1 0x0400
#define PAD_R1 0x0800
#define PAD_TRIANGLE 0x1000
#define PAD_CIRCLE 0x2000
#define PAD_CROSS 0x4000
#define PAD_SQUARE 0x8000

struct JoyInfo
{
    uint8_t padBuf[256];
    uint8_t port;
    uint8_t slot;
    int8_t opened;
} __attribute__((aligned(64)));

struct JoyInfo joyInfo[MAX_CONTROLLERS];

static uint8_t enabled_pads;

// Map PS2 button state to PSX digital controller protocol (ID + 2 bytes for buttons)
// PSX expects: 0x41 (ID), then 2 bytes: [low, high] (0=pressed)
// Button mapping: https://problemkaputt.de/psx-spx.htm#controllersioports
// PS2 padButtonStatus: https://ps2dev.github.io/ps2sdk/ps2sdk/libpad_8h.html
// This function fills a 3-byte buffer with the PSX controller response
void Joystick_GetPSXDigitalResponse(uint8_t response[3])
{
    uint32_t ps2 = Joystick_Poll();
    response[0] = 0x41; // Digital pad ID
    response[1] = 0xFF;
    response[2] = 0xFF;

    // PSX low byte: 0x01=Select, 0x02=L3, 0x04=R3, 0x08=Start, 0x10=Up, 0x20=Right, 0x40=Down, 0x80=Left
    // PSX high byte: 0x01=L2, 0x02=R2, 0x04=L1, 0x08=R1, 0x10=Triangle, 0x20=Circle, 0x40=Cross, 0x80=Square

    if (ps2 & PAD_SELECT)
        response[1] &= ~0x01;
    if (ps2 & PAD_L3)
        response[1] &= ~0x02;
    if (ps2 & PAD_R3)
        response[1] &= ~0x04;
    if (ps2 & PAD_START)
        response[1] &= ~0x08;
    if (ps2 & PAD_UP)
        response[1] &= ~0x10;
    if (ps2 & PAD_RIGHT)
        response[1] &= ~0x20;
    if (ps2 & PAD_DOWN)
        response[1] &= ~0x40;
    if (ps2 & PAD_LEFT)
        response[1] &= ~0x80;

    if (ps2 & PAD_L2)
        response[2] &= ~0x01;
    if (ps2 & PAD_R2)
        response[2] &= ~0x02;
    if (ps2 & PAD_L1)
        response[2] &= ~0x04;
    if (ps2 & PAD_R1)
        response[2] &= ~0x08;
    if (ps2 & PAD_TRIANGLE)
        response[2] &= ~0x10;
    if (ps2 & PAD_CIRCLE)
        response[2] &= ~0x20;
    if (ps2 & PAD_CROSS)
        response[2] &= ~0x40;
    if (ps2 & PAD_SQUARE)
        response[2] &= ~0x80;
}

void Joystick_Init(void)
{
    uint32_t port = 0;
    uint32_t slot = 0;

    if (init_joystick_driver(true) < 0)
    {
        printf("ERROR: Failed to initialize joystick driver!\n");
        return;
    }

    for (port = 0; port < PS2_MAX_PORT; port++)
    {
        mtapPortOpen(port);
    }
    /* it can fail - we dont care, we will check it more strictly when padPortOpen */

    for (slot = 0; slot < PS2_MAX_SLOT; slot++)
    {
        for (port = 0; port < PS2_MAX_PORT; port++)
        {
            /* 2 main controller ports acts the same with and without multitap
            Port 0,0 -> Connector 1 - the same as Port 0
            Port 1,0 -> Connector 2 - the same as Port 1
            Port 0,1 -> Connector 3
            Port 1,1 -> Connector 4
            Port 0,2 -> Connector 5
            Port 1,2 -> Connector 6
            Port 0,3 -> Connector 7
            Port 1,3 -> Connector 8
            */

            struct JoyInfo *info = &joyInfo[enabled_pads];
            if (padPortOpen(port, slot, (void *)info->padBuf) > 0)
            {
                info->port = (uint8_t)port;
                info->slot = (uint8_t)slot;
                info->opened = 1;
                enabled_pads++;
            }
        }
    }
}

void Joystick_Shutdown(void)
{
    uint32_t i = 0;

    for (i = 0; i < MAX_CONTROLLERS; i++)
    {
        struct JoyInfo *info = &joyInfo[i];
        if (info->opened)
        {
            padPortClose(info->port, info->slot);
        }
    }

    deinit_joystick_driver(true);
}

static struct JoyInfo *getFirstOpenJoyInfo(void)
{
    uint32_t i;

    for (i = 0; i < MAX_CONTROLLERS; i++)
    {
        if (joyInfo[i].opened)
            return &joyInfo[i];
    }

    return NULL;
}

static uint32_t basicPoll(void)
{
    uint32_t data = 0;
    int32_t state, ret;
    struct padButtonStatus paddata;
    struct JoyInfo *info;

    info = getFirstOpenJoyInfo();
    if (info == NULL)
        return 0;

    state = padGetState(info->port, info->slot);
    if (state != PAD_STATE_DISCONN && state != PAD_STATE_EXECCMD && state != PAD_STATE_ERROR)
    {
        ret = padRead(info->port, info->slot, &paddata);
        if (ret != 0)
            data = (0xFFFF ^ paddata.btns) & 0xFFFF;
    }

    return data;
}

uint32_t Joystick_Poll(void)
{
    if (enabled_pads == 0)
        return 0;

    return basicPoll();
}
