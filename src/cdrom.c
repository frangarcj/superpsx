/*
 * SuperPSX - CD-ROM Controller Emulation
 *
 * Emulates the PSX CD-ROM controller with disc-present simulation.
 * Supports: GetStat, Setloc, SeekL, SeekP, ReadN, Pause, Init,
 *           Demute, SetMode, GetlocL, GetlocP, GetID, Test, ReadTOC.
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
#define DATA_FIFO_SIZE 2048

/* ---- Disc geometry ---- */
#define DISC_MAX_LBA 335250 /* 74:30:00 - seeks beyond this fail */
#define LEADOUT_LBA  333000 /* 74:00:00 - lead-out area starts here */
#define PREGAP_LBA   150    /* 00:02:00 - data area starts here */

/* ---- BCD helpers ---- */
static u8 dec_to_bcd(int v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static int bcd_to_dec(u8 v) { return (v >> 4) * 10 + (v & 0x0F); }

static u32 msf_to_lba(u8 mm, u8 ss, u8 ff)
{
    return (u32)(bcd_to_dec(mm) * 60 + bcd_to_dec(ss)) * 75 + bcd_to_dec(ff);
}

static void lba_to_bcd(u32 lba, u8 *mm, u8 *ss, u8 *ff)
{
    *ff = dec_to_bcd(lba % 75); lba /= 75;
    *ss = dec_to_bcd(lba % 60); lba /= 60;
    *mm = dec_to_bcd(lba);
}

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

    /* Data FIFO (sector data for CPU) */
    u8 data_fifo[DATA_FIFO_SIZE];
    u32 data_pos;
    u32 data_len;

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

    /* Disc position tracking */
    u32 setloc_lba;      /* Target set by Setloc (in LBA sectors) */
    u32 cur_lba;          /* Current head position (LBA) */
    u8  has_loc_header;   /* 1 = GetlocL can return data */
    u8  seek_error;       /* 1 = last seek failed */
    u8  reading;          /* 1 = ReadN/ReadS in progress */
    u8  mode;             /* SetMode value */
    s32 read_delay;       /* Cycles until next INT1 delivery */
    s32 pending_delay;    /* Cycles until pending response delivery */
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
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;

    case 0x02: /* Setloc (MM, SS, FF) - BCD parameters */
    {
        u8 mm = (cdrom.param_count > 0) ? cdrom.param_fifo[0] : 0;
        u8 ss = (cdrom.param_count > 1) ? cdrom.param_fifo[1] : 0;
        u8 ff = (cdrom.param_count > 2) ? cdrom.param_fifo[2] : 0;
        cdrom.setloc_lba = msf_to_lba(mm, ss, ff);
        printf("[CDROM] Cmd 02h Setloc(%02X:%02X:%02X) -> LBA %u\n",
               mm, ss, ff, cdrom.setloc_lba);
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;
    }

    case 0x06: /* ReadN - Read with retry */
    {
        printf("[CDROM] Cmd 06h ReadN from LBA %u\n", cdrom.setloc_lba);
        cdrom.cur_lba = cdrom.setloc_lba;
        cdrom.reading = 1;
        cdrom.has_loc_header = 1;
        cdrom.seek_error = 0;
        cdrom.stat = 0x42; /* Seeking + Motor On */
        cdrom.read_delay = 10000000; /* ~300ms: long enough for test to observe stat=0x42 */
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 acknowledge */
        /* INT1 (data ready) will be delivered by CDROM_Update */
        break;
    }

    case 0x09: /* Pause */
        printf("[CDROM] Cmd 09h Pause\n");
        cdrom.reading = 0;
        cdrom.stat = 0x02; /* Motor On, idle */
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        resp[0] = cdrom.stat;
        cdrom_queue_pending(resp, 1, 2); /* INT2 */
        break;

    case 0x0A: /* Init / Reset */
    {
        printf("[CDROM] Cmd 0Ah Init\n");
        u8 had_header = cdrom.has_loc_header;
        cdrom.reading = 0;
        cdrom.seek_error = 0;
        cdrom.stat = 0x02; /* Motor On, idle */
        if (had_header)
        {
            /* Head moves to inner area but header data still available */
            cdrom.cur_lba = PREGAP_LBA; /* 00:02:00 */
            cdrom.has_loc_header = 1;
        }
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        resp[0] = cdrom.stat;
        cdrom_queue_pending(resp, 1, 2); /* INT2 */
        break;
    }

    case 0x0C: /* Demute */
        printf("[CDROM] Cmd 0Ch Demute\n");
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;

    case 0x0E: /* SetMode */
        cdrom.mode = (cdrom.param_count > 0) ? cdrom.param_fifo[0] : 0;
        printf("[CDROM] Cmd 0Eh SetMode(%02X)\n", cdrom.mode);
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;

    case 0x10: /* GetlocL - Get logical position (sector header) */
        if (!cdrom.has_loc_header || cdrom.seek_error)
        {
            printf("[CDROM] Cmd 10h GetlocL -> FAIL (no header)\n");
            resp[0] = cdrom.stat;
            resp[1] = 0x80; /* Invalid argument / no data */
            cdrom_queue_response(resp, 2, 5); /* INT5 error */
        }
        else
        {
            u8 mm, ss, ff;
            lba_to_bcd(cdrom.cur_lba, &mm, &ss, &ff);
            printf("[CDROM] Cmd 10h GetlocL -> %02X:%02X:%02X mode 2\n",
                   mm, ss, ff);
            resp[0] = mm;       /* Absolute minute (BCD) */
            resp[1] = ss;       /* Absolute second (BCD) */
            resp[2] = ff;       /* Absolute frame (BCD) */
            resp[3] = 0x02;     /* Mode 2 */
            resp[4] = 0x00;     /* File */
            resp[5] = 0x00;     /* Channel */
            resp[6] = 0x00;     /* Sub-mode */
            resp[7] = 0x00;     /* Coding info */
            cdrom_queue_response(resp, 8, 3); /* INT3 */
        }
        break;

    case 0x11: /* GetlocP - Get physical position (subchannel Q) */
        if (cdrom.seek_error)
        {
            printf("[CDROM] Cmd 11h GetlocP -> FAIL (seek error)\n");
            resp[0] = cdrom.stat;
            resp[1] = 0x80;
            cdrom_queue_response(resp, 2, 5); /* INT5 error */
        }
        else
        {
            u8 amm, ass, aff; /* absolute */
            u8 rmm, rss, rff; /* relative */
            lba_to_bcd(cdrom.cur_lba, &amm, &ass, &aff);

            /* Track number: 0xAA for lead-out, 0x01 for data */
            u8 track = (cdrom.cur_lba >= LEADOUT_LBA) ? 0xAA : 0x01;
            /* Index: 0x00 for pregap, 0x01 for data/lead-out */
            u8 index = (cdrom.cur_lba < PREGAP_LBA) ? 0x00 : 0x01;

            /* Relative position within track */
            if (cdrom.cur_lba >= PREGAP_LBA)
            {
                u32 rel = cdrom.cur_lba - PREGAP_LBA;
                lba_to_bcd(rel, &rmm, &rss, &rff);
            }
            else
            {
                /* Pregap: count remaining frames to data start */
                u32 rem = PREGAP_LBA - cdrom.cur_lba;
                lba_to_bcd(rem, &rmm, &rss, &rff);
            }

            printf("[CDROM] Cmd 11h GetlocP -> T%02X I%02X [%02X:%02X:%02X] abs [%02X:%02X:%02X]\n",
                   track, index, rmm, rss, rff, amm, ass, aff);
            resp[0] = track;
            resp[1] = index;
            resp[2] = rmm;
            resp[3] = rss;
            resp[4] = rff;
            resp[5] = amm;
            resp[6] = ass;
            resp[7] = aff;
            cdrom_queue_response(resp, 8, 3); /* INT3 */
        }
        break;

    case 0x15: /* SeekL - Seek (data mode) */
    case 0x16: /* SeekP - Seek (audio mode) */
    {
        printf("[CDROM] Cmd %02Xh Seek to LBA %u\n", cmd, cdrom.setloc_lba);
        if (cdrom.setloc_lba >= DISC_MAX_LBA)
        {
            /* Out of range - seek error */
            cdrom.seek_error = 1;
            cdrom.stat = 0x04; /* Seek Error */
            resp[0] = cdrom.stat;
            cdrom_queue_response(resp, 1, 3); /* INT3 */
            resp[0] = cdrom.stat;
            resp[1] = 0x04; /* Seek error code */
            cdrom_queue_pending(resp, 2, 5); /* INT5 error */
        }
        else
        {
            /* Seek succeeds */
            cdrom.cur_lba = cdrom.setloc_lba;
            cdrom.has_loc_header = 1;
            cdrom.seek_error = 0;
            cdrom.stat = 0x02; /* Motor On */
            resp[0] = cdrom.stat;
            cdrom_queue_response(resp, 1, 3); /* INT3 */
            resp[0] = cdrom.stat;
            cdrom_queue_pending(resp, 1, 2); /* INT2 complete */
        }
        break;
    }

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
    {
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
    }

    case 0x1B: /* ReadS - Read without retry */
    {
        printf("[CDROM] Cmd 1Bh ReadS from LBA %u\n", cdrom.setloc_lba);
        cdrom.cur_lba = cdrom.setloc_lba;
        cdrom.reading = 1;
        cdrom.has_loc_header = 1;
        cdrom.seek_error = 0;
        cdrom.stat = 0x42; /* Seeking + Motor On */
        cdrom.read_delay = 10000000;
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;
    }

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
        if (cdrom.data_pos < cdrom.data_len)
            status |= 0x40; /* DRQSTS - data FIFO not empty */
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
        if (cdrom.data_pos < cdrom.data_len)
        {
            result = cdrom.data_fifo[cdrom.data_pos++];
        }
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
}

/* ---- Tick the CD-ROM for timed operations ---- */
void CDROM_Update(u32 cycles)
{
    /* Deliver pending responses after delay */
    if (cdrom.pending_delay > 0)
    {
        cdrom.pending_delay -= (s32)cycles;
        if (cdrom.pending_delay <= 0)
        {
            cdrom.pending_delay = 0;
            if (cdrom.has_pending && cdrom.int_flag == 0)
                cdrom_deliver_pending();
        }
    }

    if (!cdrom.reading)
        return;

    /* Count down read delay */
    if (cdrom.read_delay > 0)
    {
        cdrom.read_delay -= (s32)cycles;
        if (cdrom.read_delay <= 0)
        {
            /* Seek complete, now in reading state */
            cdrom.stat = 0x22; /* Reading + Motor On */
        }
        return;
    }

    /* Deliver INT1 (data ready) when possible */
    if (cdrom.int_flag != 0 || cdrom.has_pending)
        return; /* Wait for previous INT to be acknowledged */

    /* Fill data FIFO with dummy sector data */
    memset(cdrom.data_fifo, 0, DATA_FIFO_SIZE);
    cdrom.data_pos = 0;
    cdrom.data_len = DATA_FIFO_SIZE;

    /* Deliver INT1 response */
    {
        u8 resp[1];
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 1); /* INT1 = data ready */
    }

    /* Advance position */
    cdrom.cur_lba++;

    /* Set delay until next sector (~225792 cycles at 33.8MHz for 150 sectors/s) */
    cdrom.read_delay = 30000;
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
                /* Request data - mark data as available */
            }
            else
            {
                /* Clear data FIFO */
                cdrom.data_pos = 0;
                cdrom.data_len = 0;
            }
            break;
        case 1: /* Interrupt Flag Register (acknowledge) */
            cdrom.int_flag &= ~(val & 0x07);
            if (val & 0x40)
            {
                /* Reset parameter FIFO */
                cdrom.param_count = 0;
            }
            /* Schedule pending delivery after a short delay so the
             * current IRQ handler can finish reading the response
             * FIFO before it's overwritten by the pending response. */
            if (cdrom.has_pending && cdrom.int_flag == 0)
            {
                cdrom.pending_delay = 200; /* ~25 instructions */
            }
            break;
        case 2: /* Audio Volume Left→Right */
            break;
        case 3: /* Apply Audio Volume changes */
            break;
        }
        break;
    }
}
