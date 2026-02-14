/*
 * SuperPSX - Minimal CD-ROM Controller Emulation
 *
 * Implements enough of the PSX CD-ROM interface for the BIOS to:
 *   1. Initialize the CD-ROM controller
 *   2. Detect "no disc" condition
 *   3. Proceed to the memory card / CD player menu
 *
 * CD-ROM registers: 0x1F801800-0x1F801803
 * Register meanings vary based on the Index (bits 0-1 of 0x1F801800)
 */

#include "superpsx.h"
#include <stdio.h>
#include <string.h>

/* ---- FIFOs ---- */
#define PARAM_FIFO_SIZE 16
#define RESPONSE_FIFO_SIZE 16

/* ---- CD-ROM State ---- */
static struct
{
    u8 index; /* Current register index (0-3) */

    /* Parameter FIFO (CPU → CD controller) */
    u8 param_fifo[PARAM_FIFO_SIZE];
    u8 param_count;

    /* Response FIFO (CD controller → CPU) */
    u8 response_fifo[RESPONSE_FIFO_SIZE];
    u8 response_count;
    u8 response_read_pos;

    /* Interrupt system */
    u8 int_enable; /* Interrupt Enable mask (5 bits) */
    u8 int_flag;   /* Current interrupt flag (1-5) */

    /* CD-ROM status byte (returned by GetStat etc.) */
    u8 stat;

    /* Pending second response (for 2-part commands like GetID) */
    u8 pending_response[RESPONSE_FIFO_SIZE];
    u8 pending_count;
    u8 pending_int; /* INT type for pending response */
    u8 has_pending; /* 1 if a second response is queued */

    /* Command tracking */
    u8 last_cmd;
    u8 busy; /* 1 = processing command */
} cdrom;

/* ---- Initialization ---- */
void CDROM_Init(void)
{
    memset(&cdrom, 0, sizeof(cdrom));
    cdrom.stat = 0x10; /* ShellOpen = no disc inserted */
    printf("[CDROM] Initialized (no disc)\n");
}

/* ---- Queue a response ---- */
static void cdrom_queue_response(const u8 *data, int count, u8 irq_type)
{
    if (count > RESPONSE_FIFO_SIZE)
        count = RESPONSE_FIFO_SIZE;
    memcpy(cdrom.response_fifo, data, count);
    cdrom.response_count = count;
    cdrom.response_read_pos = 0;
    cdrom.int_flag = irq_type;
    cdrom.busy = 0;

    /* Always signal PSX IRQ2 (CD-ROM).
     * The CD-ROM int_enable only controls which INT types
     * trigger the IRQ, but we always assert it to be safe.
     * The BIOS checks I_STAT/I_MASK at the CPU level. */
    //printf("[CDROM] Queue response: %d bytes, INT%d, int_en=%02X\n",
    //       count, irq_type, cdrom.int_enable);
    SignalInterrupt(2);
    /* Debug: check if IRQ2 will be delivered */
    //{
    //    extern u32 ReadHardware(u32);
    //    printf("[CDROM] After signal: I_STAT has bit2=%d\n",
    //           (CheckInterrupts() >> 2) & 1);
    //}
}

/* ---- Queue a pending (second) response ---- */
static void cdrom_queue_pending(const u8 *data, int count, u8 irq_type)
{
    if (count > RESPONSE_FIFO_SIZE)
        count = RESPONSE_FIFO_SIZE;
    memcpy(cdrom.pending_response, data, count);
    cdrom.pending_count = count;
    cdrom.pending_int = irq_type;
    cdrom.has_pending = 1;
}

/* ---- Execute a CD-ROM command ---- */
static void cdrom_execute_command(u8 cmd)
{
    u8 resp[16];

    cdrom.last_cmd = cmd;

    switch (cmd)
    {

    case 0x01: /* GetStat */
        //printf("[CDROM] Cmd 01h GetStat → stat=%02X\n", cdrom.stat);
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;

    case 0x02: /* Setloc (MM, SS, FF) */
        printf("[CDROM] Cmd 02h Setloc\n");
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;

    case 0x06: /* ReadN */
        printf("[CDROM] Cmd 06h ReadN (no disc)\n");
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 first */
        /* Second response: INT5 error (no disc) */
        resp[0] = cdrom.stat | 0x01;     /* Error flag */
        resp[1] = 0x80;                  /* ErrorCode: no disc */
        cdrom_queue_pending(resp, 2, 5); /* INT5 */
        break;

    case 0x09: /* Pause */
        printf("[CDROM] Cmd 09h Pause\n");
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        /* Second response INT2 */
        resp[0] = cdrom.stat;
        cdrom_queue_pending(resp, 1, 2); /* INT2 */
        break;

    case 0x0A: /* Init / Reset */
        printf("[CDROM] Cmd 0Ah Init\n");
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        /* Second response INT2 */
        resp[0] = cdrom.stat;
        cdrom_queue_pending(resp, 1, 2); /* INT2 */
        break;

    case 0x0C: /* Demute */
        printf("[CDROM] Cmd 0Ch Demute\n");
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;

    case 0x0E: /* SetMode */
        printf("[CDROM] Cmd 0Eh SetMode(%02X)\n",
               cdrom.param_count > 0 ? cdrom.param_fifo[0] : 0);
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;

    case 0x15: /* SeekL */
        printf("[CDROM] Cmd 15h SeekL\n");
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        resp[0] = cdrom.stat;
        cdrom_queue_pending(resp, 1, 2); /* INT2 */
        break;

    case 0x19: /* Test - sub-function in param[0] */
    {
        u8 sub = (cdrom.param_count > 0) ? cdrom.param_fifo[0] : 0;
        printf("[CDROM] Cmd 19h Test(%02X)\n", sub);
        switch (sub)
        {
        case 0x20:                            /* Get CD-ROM BIOS date/version */
            resp[0] = 0x94;                   /* year  (1994) */
            resp[1] = 0x09;                   /* month (September) */
            resp[2] = 0x19;                   /* day   (19th) */
            resp[3] = 0xC0;                   /* version */
            cdrom_queue_response(resp, 4, 3); /* INT3 */
            break;
        case 0x04: /* Reset SCEx counters */
        case 0x05: /* Read SCEx counters */
            resp[0] = 0;
            resp[1] = 0;
            cdrom_queue_response(resp, 2, 3); /* INT3 */
            break;
        default:
            resp[0] = cdrom.stat;
            cdrom_queue_response(resp, 1, 3); /* INT3 */
            break;
        }
        break;
    }

    case 0x1A: /* GetID - Disc identification */
        printf("[CDROM] Cmd 1Ah GetID (no disc)\n");
        /* First response: INT3 */
        resp[0] = cdrom.stat;
        resp[1] = 0x00;
        cdrom_queue_response(resp, 2, 3); /* INT3 */
        /* Second response: INT5 (error - no disc) */
        resp[0] = 0x08; /* stat: ShellOpen */
        resp[1] = 0x40; /* flags: Missing Disc */
        resp[2] = 0x00;
        resp[3] = 0x00;
        resp[4] = 0x00; /* No SCEx string */
        resp[5] = 0x00;
        resp[6] = 0x00;
        resp[7] = 0x00;
        cdrom_queue_pending(resp, 8, 5); /* INT5 error */
        break;

    case 0x1E: /* ReadTOC */
        printf("[CDROM] Cmd 1Eh ReadTOC\n");
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        resp[0] = cdrom.stat;
        cdrom_queue_pending(resp, 1, 2); /* INT2 */
        break;

    default:
        printf("[CDROM] Unknown Cmd %02Xh\n", cmd);
        /* Return INT5 error for unknown commands */
        resp[0] = cdrom.stat | 0x01;
        resp[1] = 0x40;                   /* Invalid command */
        cdrom_queue_response(resp, 2, 5); /* INT5 */
        break;
    }

    /* Clear parameter FIFO after command execution */
    cdrom.param_count = 0;
}

/* ---- Deliver pending response (called after INT acknowledge) ---- */
static void cdrom_deliver_pending(void)
{
    if (!cdrom.has_pending)
        return;
    if (cdrom.int_flag != 0)
    {
        //printf("[CDROM] Pending delivery blocked: int_flag=%02X (need 0)\n", cdrom.int_flag);
        return; /* Wait for current INT to be acknowledged */
    }

    //printf("[CDROM] Delivering pending response: %d bytes, INT%d\n",
    //       cdrom.pending_count, cdrom.pending_int);
    memcpy(cdrom.response_fifo, cdrom.pending_response, cdrom.pending_count);
    cdrom.response_count = cdrom.pending_count;
    cdrom.response_read_pos = 0;
    cdrom.int_flag = cdrom.pending_int;
    cdrom.has_pending = 0;

    SignalInterrupt(2); /* CD-ROM IRQ */
}

/* ---- Read CD-ROM register ---- */
u32 CDROM_Read(u32 addr)
{
    u32 reg = addr & 3;
    u32 result = 0;

    switch (reg)
    {
    case 0: /* 0x1F801800 - Status Register */
    {
        u8 status = cdrom.index & 3;
        if (cdrom.param_count == 0)
            status |= 0x08;
        if (cdrom.param_count < PARAM_FIFO_SIZE)
            status |= 0x10;
        if (cdrom.response_read_pos < cdrom.response_count)
            status |= 0x20;
        if (cdrom.busy)
            status |= 0x80;
        result = status;
        break;
    }

    case 1: /* 0x1F801801 - Response FIFO (all indices) */
        if (cdrom.response_read_pos < cdrom.response_count)
        {
            result = cdrom.response_fifo[cdrom.response_read_pos++];
        }
        break;

    case 2: /* 0x1F801802 - Data FIFO (all indices) */
        break;

    case 3: /* 0x1F801803 - Interrupt Enable or Interrupt Flag */
        if (cdrom.index & 1)
        {
            result = cdrom.int_flag | 0xE0;
        }
        else
        {
            result = cdrom.int_enable;
        }
        break;
    }

    /* Log non-status reads (avoid flooding from status polling) */
    //if (reg != 0)
    //{
    //    printf("[CDROM] Read reg%d (idx=%d) = %02X\n", reg, cdrom.index, result);
    //}
    return result;

    return 0;
}

/* ---- Write CD-ROM register ---- */
void CDROM_Write(u32 addr, u32 data)
{
    u32 reg = addr & 3;
    u8 val = data & 0xFF;

    //printf("[CDROM] Write reg%d (idx=%d) val=%02X\n", reg, cdrom.index, val);

    switch (reg)
    {
    case 0: /* 0x1F801800 - Index Register */
        cdrom.index = val & 3;
        break;

    case 1: /* 0x1F801801 */
        switch (cdrom.index)
        {
        case 0: /* Command Register */
            cdrom.busy = 1;
            cdrom_execute_command(val);
            break;
        case 1: /* Sound Map Data Out */
            break;
        case 2: /* Sound Map Coding Info */
            break;
        case 3: /* Audio Volume Right→Left */
            break;
        }
        break;

    case 2: /* 0x1F801802 */
        switch (cdrom.index)
        {
        case 0: /* Parameter FIFO */
            if (cdrom.param_count < PARAM_FIFO_SIZE)
            {
                cdrom.param_fifo[cdrom.param_count++] = val;
            }
            break;
        case 1: /* Interrupt Enable Register */
            cdrom.int_enable = val & 0x1F;
            break;
        case 2: /* Audio Volume Left→Left */
            break;
        case 3: /* Audio Volume Right→Right */
            break;
        }
        break;

    case 3: /* 0x1F801803 */
        switch (cdrom.index)
        {
        case 0: /* Request Register */
            if (val & 0x80)
            {
                /* Request data - not supported (no disc) */
            }
            break;
        case 1: /* Interrupt Flag Register (acknowledge) */
            cdrom.int_flag &= ~(val & 0x07);
            if (val & 0x40)
            {
                /* Reset parameter FIFO */
                cdrom.param_count = 0;
            }
            /* After acknowledging, deliver pending response */
            cdrom_deliver_pending();
            break;
        case 2: /* Audio Volume Left→Right */
            break;
        case 3: /* Apply Audio Volume changes */
            break;
        }
        break;
    }
}
