#include <stdio.h>
#include <stdint.h>
#include "superpsx.h"
#include "scheduler.h"
#include "joystick.h"
#include "psx_sio.h"

#define LOG_TAG "SIO"

/* Joypad/Memcard interface */
static uint32_t sio_data = 0xFF;       /* RX Data register */
static uint32_t sio_stat = 0x00000005; /* TX Ready 1+2 */
static uint16_t sio_mode = 0;
static uint16_t sio_ctrl = 0;
static uint16_t sio_baud = 0;
static int sio_tx_pending = 0; /* 1 = RX data available */

/* Controller protocol state machine */
static int sio_state = 0;        /* Current byte index in protocol */
static uint8_t sio_response[20]; /* Pre-built response buffer */
static int sio_response_len = 0; /* Number of valid bytes in sio_response */
static int sio_selected = 0;     /* 1 = JOY SELECT is asserted */
static int sio_port = 0;         /* 0 = PSX port 1, 1 = PSX port 2 */

/* SIO Serial Port (0x1F801050-0x1F80105E) */
static uint16_t serial_mode = 0;
static uint16_t serial_ctrl = 0;
static uint16_t serial_baud = 0;

#define SIO_IRQ_DELAY 500
volatile uint64_t sio_irq_delay_cycle = 0;
static int sio_irq_pending = 0;

uint32_t SIO_Read(uint32_t addr)
{
    uint32_t phys = addr & 0x1FFFFFFF;

    if (phys == 0x1F801040)
    {
        uint32_t val = sio_data;
        sio_tx_pending = 0;
        return val;
    }
    if (phys == 0x1F801044)
    {
        uint32_t stat = 0x00000005;
        if (sio_tx_pending)
            stat |= 0x02;
        if (sio_selected && sio_state > 0 && sio_state < sio_response_len)
            stat |= 0x80;
        stat |= (sio_stat & (1 << 9));
        return stat;
    }
    if (phys == 0x1F801048)
        return sio_mode & 0x003F;
    if (phys == 0x1F80104A)
        return sio_ctrl;
    if (phys == 0x1F80104E)
        return sio_baud;

    if (phys == 0x1F801050)
        return 0xFF;
    if (phys == 0x1F801054)
        return 0x00000005;
    if (phys == 0x1F801058)
        return serial_mode & 0xFF;
    if (phys == 0x1F80105A)
        return serial_ctrl;
    if (phys == 0x1F80105E)
        return serial_baud;

    return 0;
}

void SIO_Write(uint32_t addr, uint32_t data)
{
    uint32_t phys = addr & 0x1FFFFFFF;

    if (phys == 0x1F801040)
    {
        uint8_t tx = (uint8_t)(data & 0xFF);
        if (!sio_selected)
        {
            sio_data = 0xFF;
            sio_tx_pending = 1;
            return;
        }

        if (sio_state == 0)
        {
            if (tx == 0x01)
            {
                if (Joystick_HasMultitap(sio_port))
                {
                    int slot;
                    sio_response[0] = 0xFF;
                    sio_response[1] = 0x80;
                    sio_response[2] = 0x5A;
                    for (slot = 0; slot < 4; slot++)
                    {
                        int base = 3 + slot * 4;
                        if (Joystick_IsConnected(sio_port, slot))
                        {
                            uint8_t pad[3];
                            Joystick_GetPSXDigitalResponse(sio_port, slot, pad);
                            sio_response[base]     = pad[0];
                            sio_response[base + 1] = 0x5A;
                            sio_response[base + 2] = pad[1];
                            sio_response[base + 3] = pad[2];
                        }
                        else
                        {
                            sio_response[base]     = 0xFF;
                            sio_response[base + 1] = 0xFF;
                            sio_response[base + 2] = 0xFF;
                            sio_response[base + 3] = 0xFF;
                        }
                    }
                    sio_response_len = 19;
                }
                else
                {
                    uint8_t pad[3];
                    Joystick_GetPSXDigitalResponse(sio_port, 0, pad);
                    sio_response[0] = 0xFF;
                    sio_response[1] = pad[0];
                    sio_response[2] = 0x5A;
                    sio_response[3] = pad[1];
                    sio_response[4] = pad[2];
                    sio_response_len = 5;
                }
                sio_data = sio_response[0];
                sio_state = 1;
                sio_tx_pending = 1;
                sio_irq_pending = 1;
                sio_irq_delay_cycle = global_cycles + SIO_IRQ_DELAY;
            }
            else
            {
                sio_data = 0xFF;
                sio_tx_pending = 1;
            }
        }
        else if (sio_state < sio_response_len)
        {
            sio_data = sio_response[sio_state];
            sio_tx_pending = 1;
            if (sio_state < sio_response_len - 1)
            {
                sio_irq_pending = 1;
                sio_irq_delay_cycle = global_cycles + SIO_IRQ_DELAY;
            }
            sio_state++;
        }
        else
        {
            sio_data = 0xFF;
            sio_tx_pending = 1;
        }
        return;
    }
    if (phys == 0x1F801048)
    {
        sio_mode = (uint16_t)(data & 0x003F);
        return;
    }
    if (phys == 0x1F80104A)
    {
        sio_ctrl = (uint16_t)data;
        if (data & 0x40)
        {
            sio_ctrl = 0; sio_mode = 0; sio_baud = 0;
            sio_tx_pending = 0; sio_state = 0;
            sio_response_len = 0; sio_selected = 0;
            sio_port = 0; sio_data = 0xFF;
            sio_irq_pending = 0; sio_irq_delay_cycle = 0;
            return;
        }
        if (data & 0x10)
        {
            sio_stat &= ~(1 << 9);
            if (sio_irq_pending)
            {
                sio_irq_delay_cycle = 0;
                psx_abort_pc = cpu.current_pc + 4;
                cpu.block_aborted = 1;
            }
        }
        sio_port = (data >> 13) & 1;
        if (data & 0x02)
        {
            if (!sio_selected) sio_state = 0;
            sio_selected = 1;
        }
        else
        {
            sio_selected = 0; sio_state = 0;
            sio_irq_pending = 0; sio_irq_delay_cycle = 0;
        }
        return;
    }
    if (phys == 0x1F80104E)
    {
        sio_baud = (uint16_t)data;
        return;
    }

    if (phys == 0x1F801058)
    {
        serial_mode = (uint16_t)(data & 0xFF);
        return;
    }
    if (phys == 0x1F80105A)
    {
        serial_ctrl = (uint16_t)data;
        if (data & 0x40) { serial_ctrl = 0; serial_mode = 0; serial_baud = 0; }
        return;
    }
    if (phys == 0x1F80105E)
    {
        serial_baud = (uint16_t)data;
        return;
    }
}

/* SIO IRQ check for I_STAT (called by hardware.c) */
void SIO_CheckIRQ(uint32_t data)
{
    if (sio_irq_pending && !(data & (1 << 7)))
    {
        sio_irq_pending = 0;
        sio_irq_delay_cycle = 0;
        SignalInterrupt(7);
    }
}
