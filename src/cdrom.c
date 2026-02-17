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
#include "scheduler.h"
#include "iso_image.h"
#include <stdio.h>
#include <string.h>

#define LOG_TAG "CDROM"

/* ---- FIFOs ---- */
#define PARAM_FIFO_SIZE 16
#define RESPONSE_FIFO_SIZE 16
#define DATA_FIFO_SIZE 2352 /* Large enough for raw sector mode (2340 bytes) */

/* ---- Disc geometry ---- */
#define DISC_MAX_LBA 335250 /* 74:30:00 - seeks beyond this fail */
#define LEADOUT_LBA 333000  /* 74:00:00 - lead-out area starts here */
#define PREGAP_LBA 150      /* 00:02:00 - data area starts here */

/* ---- BCD helpers ---- */
static uint8_t dec_to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int bcd_to_dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

static uint32_t msf_to_lba(uint8_t mm, uint8_t ss, uint8_t ff)
{
    return (uint32_t)(bcd_to_dec(mm) * 60 + bcd_to_dec(ss)) * 75 + bcd_to_dec(ff);
}

static void lba_to_bcd(uint32_t lba, uint8_t *mm, uint8_t *ss, uint8_t *ff)
{
    *ff = dec_to_bcd(lba % 75);
    lba /= 75;
    *ss = dec_to_bcd(lba % 60);
    lba /= 60;
    *mm = dec_to_bcd(lba);
}

/* ---- CD-ROM State ---- */
static struct
{
    uint8_t index; /* Current register index (0-3) */

    /* Parameter FIFO (CPU → CD controller) */
    uint8_t param_fifo[PARAM_FIFO_SIZE];
    uint8_t param_count;

    /* Response FIFO (CD controller → CPU) */
    uint8_t response_fifo[RESPONSE_FIFO_SIZE];
    uint8_t response_count;
    uint8_t response_read_pos;

    /* Data FIFO (sector data for CPU) */
    uint8_t data_fifo[DATA_FIFO_SIZE];
    uint32_t data_pos;
    uint32_t data_len;

    /* Interrupt system */
    uint8_t int_enable; /* Interrupt Enable mask (5 bits) */
    uint8_t int_flag;   /* Current interrupt flag (1-5) */

    /* CD-ROM status byte (returned by GetStat etc.) */
    uint8_t stat;

    /* Pending second response (for 2-part commands like GetID) */
    uint8_t pending_response[RESPONSE_FIFO_SIZE];
    uint8_t pending_count;
    uint8_t pending_int; /* INT type for pending response */
    uint8_t has_pending; /* 1 if a second response is queued */

    /* Command tracking */
    uint8_t last_cmd;
    uint8_t busy; /* 1 = processing command */

    /* Disc position tracking */
    uint32_t setloc_lba;    /* Target set by Setloc (in LBA sectors) */
    uint32_t cur_lba;       /* Current head position (LBA) */
    uint8_t has_loc_header; /* 1 = GetlocL can return data */
    uint8_t seek_error;     /* 1 = last seek failed */
    uint8_t reading;        /* 1 = ReadN/ReadS in progress */
    uint8_t mode;           /* SetMode value */
    uint8_t disc_present;   /* 0 = no disc (ShellOpen), 1 = disc inserted */
    int32_t read_delay;     /* Cycles until next INT1 delivery (legacy, used for state) */
    int32_t pending_delay;  /* Cycles until pending response delivery (legacy) */
    uint8_t seek_pending;   /* 1 = seek is in progress, waiting for scheduler */

    /* Deferred first response (INT3) — mimics real CD controller latency */
    uint8_t deferred_response[RESPONSE_FIFO_SIZE];
    uint8_t deferred_count;
    uint8_t deferred_int;   /* INT type for deferred response */
    uint8_t has_deferred;   /* 1 if a deferred response is queued */
    int32_t deferred_delay; /* Cycles until deferred response delivery */

    /* IRQ signal delay — models propagation latency from CD-ROM controller
     * to CPU interrupt line.  On real hardware the polling loop at
     * 0x1F801803 can see int_flag a few µs before the CPU exception
     * fires, which allows poll-based CD libraries (PSXSDK) to read the
     * response before the ISR clears int_flag. */
    int32_t irq_signal_delay;
} cdrom;

/* ---- Forward declarations for scheduler ---- */
static void CDROM_EventCallback(void);
static void CDROM_PendingCallback(void);

/* ---- Update stat preserving ShellOpen when no disc ---- */
static void cdrom_set_stat(uint8_t new_stat)
{
    if (!cdrom.disc_present)
        cdrom.stat = new_stat | 0x10; /* Preserve ShellOpen */
    else
        cdrom.stat = new_stat;
}

/* ---- Initialization ---- */
void CDROM_Init(void)
{
    memset(&cdrom, 0, sizeof(cdrom));
    cdrom.disc_present = 0; /* No disc inserted */
    cdrom.stat = 0x10;      /* ShellOpen = no disc inserted */
    DLOG("Initialized (no disc)\n");
}

/* ---- Insert a disc (called when ISO is mounted) ---- */
void CDROM_InsertDisc(void)
{
    cdrom.disc_present = 1;
    cdrom.stat = 0x02; /* Motor On, idle (no ShellOpen) */
    DLOG("Disc inserted\n");
}

/* ---- Read data from the CD-ROM data FIFO (used by DMA3) ---- */
uint32_t CDROM_ReadDataFIFO(uint8_t *dst, uint32_t count)
{
    uint32_t avail = cdrom.data_len - cdrom.data_pos;
    if (count > avail)
        count = avail;
    if (count > 0)
    {
        memcpy(dst, &cdrom.data_fifo[cdrom.data_pos], count);
        cdrom.data_pos += count;
    }
    return count;
}

/* ---- Queue a response ---- */
static void cdrom_queue_response(const uint8_t *data, int count, uint8_t irq_type)
{
    if (count > RESPONSE_FIFO_SIZE)
        count = RESPONSE_FIFO_SIZE;

    /* Defer the response delivery to mimic real CD controller latency.
     * Real hardware takes ~1000-6000 cycles to process a command.
     * Without this delay, the ISR fires immediately and consumes
     * the response before the caller's polling loop starts. */
    memcpy(cdrom.deferred_response, data, count);
    cdrom.deferred_count = count;
    cdrom.deferred_int = irq_type;
    cdrom.has_deferred = 1;
    cdrom.deferred_delay = 4000; /* ~120 instructions, enough for caller to reach poll loop */
    cdrom.busy = 1;              /* Stay busy until response is delivered */
}

/* ---- Queue a pending (second) response ---- */
static void cdrom_queue_pending(const uint8_t *data, int count, uint8_t irq_type)
{
    if (count > RESPONSE_FIFO_SIZE)
        count = RESPONSE_FIFO_SIZE;
    memcpy(cdrom.pending_response, data, count);
    cdrom.pending_count = count;
    cdrom.pending_int = irq_type;
    cdrom.has_pending = 1;
}

/* ---- Execute a CD-ROM command ---- */
static void cdrom_execute_command(uint8_t cmd)
{
    uint8_t resp[16];

    cdrom.last_cmd = cmd;

    switch (cmd)
    {

    case 0x01: /* GetStat */
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;

    case 0x02: /* Setloc (MM, SS, FF) - BCD parameters */
    {
        uint8_t mm = (cdrom.param_count > 0) ? cdrom.param_fifo[0] : 0;
        uint8_t ss = (cdrom.param_count > 1) ? cdrom.param_fifo[1] : 0;
        uint8_t ff = (cdrom.param_count > 2) ? cdrom.param_fifo[2] : 0;
        cdrom.setloc_lba = msf_to_lba(mm, ss, ff);
        DLOG("Cmd 02h Setloc(%02X:%02X:%02X) -> LBA %" PRIu32 "\n",
             mm, ss, ff, cdrom.setloc_lba);
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;
    }

    case 0x03: /* Play - CDDA audio playback (stub) */
    {
        DLOG("Cmd 03h Play (stub)\n");
        cdrom.cur_lba = cdrom.setloc_lba;
        cdrom_set_stat(0x82); /* Playing + Motor On */
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;
    }

    case 0x06: /* ReadN - Read with retry */
    {
        DLOG("Cmd 06h ReadN from LBA %" PRIu32 "\n", cdrom.setloc_lba);
        cdrom.cur_lba = cdrom.setloc_lba;
        cdrom.reading = 1;
        cdrom.has_loc_header = 1;
        cdrom.seek_error = 0;
        cdrom_set_stat(0x42); /* Seeking + Motor On */
        cdrom.seek_pending = 1;
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 acknowledge */
        /* Schedule seek completion: ~300ms for initial seek */
        Scheduler_ScheduleEvent(SCHED_EVENT_CDROM,
                                global_cycles + 10000000ULL,
                                CDROM_EventCallback);
        break;
    }

    case 0x07: /* MotorOn */
        DLOG("Cmd 07h MotorOn\n");
        cdrom_set_stat(0x02); /* Motor On */
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        resp[0] = cdrom.stat;
        cdrom_queue_pending(resp, 1, 2); /* INT2 */
        break;

    case 0x08: /* Stop */
        DLOG("Cmd 08h Stop\n");
        cdrom.reading = 0;
        cdrom_set_stat(0x00); /* Motor Off */
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        resp[0] = cdrom.stat;
        cdrom_queue_pending(resp, 1, 2); /* INT2 */
        break;

    case 0x09: /* Pause */
        DLOG("Cmd 09h Pause\n");
        cdrom.reading = 0;
        cdrom_set_stat(0x02); /* Motor On, idle */
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        resp[0] = cdrom.stat;
        cdrom_queue_pending(resp, 1, 2); /* INT2 */
        break;

    case 0x0A: /* Init / Reset */
    {
        DLOG("Cmd 0Ah Init\n");
        uint8_t had_header = cdrom.has_loc_header;
        cdrom.reading = 0;
        cdrom.seek_error = 0;
        cdrom_set_stat(0x02); /* Motor On, idle (preserves ShellOpen if no disc) */
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
        DLOG("Cmd 0Ch Demute\n");
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;

    case 0x0B: /* Mute */
        DLOG("Cmd 0Bh Mute\n");
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;

    case 0x0D: /* SetFilter */
    {
        uint8_t file = (cdrom.param_count > 0) ? cdrom.param_fifo[0] : 0;
        uint8_t channel = (cdrom.param_count > 1) ? cdrom.param_fifo[1] : 0;
        DLOG("Cmd 0Dh SetFilter(file=%02X, channel=%02X)\n", file, channel);
        (void)file;
        (void)channel;
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;
    }

    case 0x0E: /* SetMode */
        cdrom.mode = (cdrom.param_count > 0) ? cdrom.param_fifo[0] : 0;
        DLOG("Cmd 0Eh SetMode(%02X)\n", cdrom.mode);
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        break;

    case 0x0F: /* GetParam */
        DLOG("Cmd 0Fh GetParam\n");
        resp[0] = cdrom.stat;
        resp[1] = cdrom.mode;
        resp[2] = 0x00;                   /* file */
        resp[3] = 0x00;                   /* channel */
        resp[4] = 0x00;                   /* ci (match) */
        resp[5] = 0x00;                   /* ci (mask) */
        cdrom_queue_response(resp, 6, 3); /* INT3 */
        break;

    case 0x10: /* GetlocL - Get logical position (sector header) */
        if (!cdrom.has_loc_header || cdrom.seek_error)
        {
            DLOG("Cmd 10h GetlocL -> FAIL (no header)\n");
            resp[0] = cdrom.stat;
            resp[1] = 0x80;                   /* Invalid argument / no data */
            cdrom_queue_response(resp, 2, 5); /* INT5 error */
        }
        else
        {
            uint8_t mm, ss, ff;
            lba_to_bcd(cdrom.cur_lba, &mm, &ss, &ff);
            DLOG("Cmd 10h GetlocL -> %02X:%02X:%02X mode 2\n",
                 mm, ss, ff);
            resp[0] = mm;                     /* Absolute minute (BCD) */
            resp[1] = ss;                     /* Absolute second (BCD) */
            resp[2] = ff;                     /* Absolute frame (BCD) */
            resp[3] = 0x02;                   /* Mode 2 */
            resp[4] = 0x00;                   /* File */
            resp[5] = 0x00;                   /* Channel */
            resp[6] = 0x00;                   /* Sub-mode */
            resp[7] = 0x00;                   /* Coding info */
            cdrom_queue_response(resp, 8, 3); /* INT3 */
        }
        break;

    case 0x11: /* GetlocP - Get physical position (subchannel Q) */
        if (cdrom.seek_error)
        {
            DLOG("Cmd 11h GetlocP -> FAIL (seek error)\n");
            resp[0] = cdrom.stat;
            resp[1] = 0x80;
            cdrom_queue_response(resp, 2, 5); /* INT5 error */
        }
        else
        {
            uint8_t amm, ass, aff; /* absolute */
            uint8_t rmm, rss, rff; /* relative */
            lba_to_bcd(cdrom.cur_lba, &amm, &ass, &aff);

            /* Track number: 0xAA for lead-out, 0x01 for data */
            uint8_t track = (cdrom.cur_lba >= LEADOUT_LBA) ? 0xAA : 0x01;
            /* Index: 0x00 for pregap, 0x01 for data/lead-out */
            uint8_t index = (cdrom.cur_lba < PREGAP_LBA) ? 0x00 : 0x01;

            /* Relative position within track */
            if (cdrom.cur_lba >= PREGAP_LBA)
            {
                uint32_t rel = cdrom.cur_lba - PREGAP_LBA;
                lba_to_bcd(rel, &rmm, &rss, &rff);
            }
            else
            {
                /* Pregap: count remaining frames to data start */
                uint32_t rem = PREGAP_LBA - cdrom.cur_lba;
                lba_to_bcd(rem, &rmm, &rss, &rff);
            }

            DLOG("Cmd 11h GetlocP -> T%02X I%02X [%02X:%02X:%02X] abs [%02X:%02X:%02X]\n",
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

    case 0x13: /* GetTN - Get first and last track numbers */
        DLOG("Cmd 13h GetTN\n");
        resp[0] = cdrom.stat;
        resp[1] = 0x01;                   /* First track: 01 (BCD) */
        resp[2] = 0x01;                   /* Last track: 01 (BCD) - single data track */
        cdrom_queue_response(resp, 3, 3); /* INT3 */
        break;

    case 0x14: /* GetTD - Get track start position */
    {
        uint8_t track = (cdrom.param_count > 0) ? cdrom.param_fifo[0] : 0;
        DLOG("Cmd 14h GetTD(track=%02X)\n", track);
        if (track == 0)
        {
            /* Track 0 = disc end (lead-out) */
            uint8_t mm, ss, ff;
            lba_to_bcd(LEADOUT_LBA + PREGAP_LBA, &mm, &ss, &ff);
            resp[0] = cdrom.stat;
            resp[1] = mm;
            resp[2] = ss;
            cdrom_queue_response(resp, 3, 3); /* INT3 */
        }
        else if (track == 1)
        {
            /* Track 1 starts at 00:02:00 (pregap) */
            resp[0] = cdrom.stat;
            resp[1] = 0x00;                   /* MM (BCD) */
            resp[2] = 0x02;                   /* SS (BCD) */
            cdrom_queue_response(resp, 3, 3); /* INT3 */
        }
        else
        {
            /* Invalid track */
            resp[0] = cdrom.stat | 0x01;
            resp[1] = 0x10;                   /* Invalid parameter */
            cdrom_queue_response(resp, 2, 5); /* INT5 */
        }
        break;
    }

    case 0x15: /* SeekL - Seek (data mode) */
    case 0x16: /* SeekP - Seek (audio mode) */
    {
        DLOG("Cmd %02Xh Seek to LBA %" PRIu32 "\n", cmd, cdrom.setloc_lba);
        if (cdrom.setloc_lba >= DISC_MAX_LBA)
        {
            /* Out of range - seek error */
            cdrom.seek_error = 1;
            cdrom_set_stat(0x04); /* Seek Error */
            resp[0] = cdrom.stat;
            cdrom_queue_response(resp, 1, 3); /* INT3 */
            resp[0] = cdrom.stat;
            resp[1] = 0x04;                  /* Seek error code */
            cdrom_queue_pending(resp, 2, 5); /* INT5 error */
        }
        else
        {
            /* Seek succeeds */
            cdrom.cur_lba = cdrom.setloc_lba;
            cdrom.has_loc_header = 1;
            cdrom.seek_error = 0;
            cdrom_set_stat(0x02); /* Motor On */
            resp[0] = cdrom.stat;
            cdrom_queue_response(resp, 1, 3); /* INT3 */
            resp[0] = cdrom.stat;
            cdrom_queue_pending(resp, 1, 2); /* INT2 complete */
        }
        break;
    }

    case 0x19: /* Test - sub-function in param[0] */
    {
        uint8_t sub = (cdrom.param_count > 0) ? cdrom.param_fifo[0] : 0;
        DLOG("Cmd 19h Test(%02X)\n", sub);
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
        if (cdrom.disc_present)
        {
            DLOG("Cmd 1Ah GetID (disc present)\n");
            /* First response: INT3 */
            resp[0] = cdrom.stat;
            cdrom_queue_response(resp, 1, 3); /* INT3 */
            /* Second response: INT2 (disc identified successfully) */
            resp[0] = cdrom.stat; /* Stat */
            resp[1] = 0x00;       /* Flags: 0x00 = data disc, licensed */
            resp[2] = 0x20;       /* Type: 0x20 = Mode2 disc */
            resp[3] = 0x00;       /* Disc type info */
            resp[4] = 'S';        /* Region: SCEA (US) */
            resp[5] = 'C';
            resp[6] = 'E';
            resp[7] = 'A';
            cdrom_queue_pending(resp, 8, 2); /* INT2 = complete */
        }
        else
        {
            DLOG("Cmd 1Ah GetID (no disc)\n");
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
        }
        break;
    }

    case 0x1B: /* ReadS - Read without retry */
    {
        DLOG("Cmd 1Bh ReadS from LBA %" PRIu32 "\n", cdrom.setloc_lba);
        cdrom.cur_lba = cdrom.setloc_lba;
        cdrom.reading = 1;
        cdrom.has_loc_header = 1;
        cdrom.seek_error = 0;
        cdrom_set_stat(0x42); /* Seeking + Motor On */
        cdrom.seek_pending = 1;
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        /* Schedule seek completion */
        Scheduler_ScheduleEvent(SCHED_EVENT_CDROM,
                                global_cycles + 10000000ULL,
                                CDROM_EventCallback);
        break;
    }

    case 0x1E: /* ReadTOC */
        DLOG("Cmd 1Eh ReadTOC\n");
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 3); /* INT3 */
        resp[0] = cdrom.stat;
        cdrom_queue_pending(resp, 1, 2); /* INT2 */
        break;

    default:
        DLOG("Unknown Cmd %02Xh\n", cmd);
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
        DLOG("Pending delivery blocked: int_flag=%02X (need 0)\n", cdrom.int_flag);
        return; /* Wait for current INT to be acknowledged */
    }

    DLOG("Delivering pending INT%d (count=%d)\n", cdrom.pending_int, cdrom.pending_count);
    memcpy(cdrom.response_fifo, cdrom.pending_response, cdrom.pending_count);
    cdrom.response_count = cdrom.pending_count;
    cdrom.response_read_pos = 0;
    cdrom.int_flag = cdrom.pending_int;
    cdrom.has_pending = 0;

    /* Delay I_STAT assertion so the polling loop can see int_flag
     * before the CPU exception fires (models real HW propagation). */
    cdrom.irq_signal_delay = 800;
}

/* ---- Deliver deferred (first) response ---- */
static void cdrom_deliver_deferred(void)
{
    if (!cdrom.has_deferred)
        return;

    DLOG("Delivering deferred INT%d (count=%d)\n", cdrom.deferred_int, cdrom.deferred_count);
    memcpy(cdrom.response_fifo, cdrom.deferred_response, cdrom.deferred_count);
    cdrom.response_count = cdrom.deferred_count;
    cdrom.response_read_pos = 0;
    cdrom.int_flag = cdrom.deferred_int;
    cdrom.has_deferred = 0;
    cdrom.busy = 0;

    /* Delay I_STAT assertion so the polling loop can see int_flag
     * before the CPU exception fires (models real HW propagation). */
    cdrom.irq_signal_delay = 800;
}

/* ---- Read CD-ROM register ---- */
uint32_t CDROM_Read(uint32_t addr)
{
    uint32_t reg = addr & 3;
    uint32_t result = 0;

    switch (reg)
    {
    case 0: /* 0x1F801800 - Status Register */
    {
        uint8_t status = cdrom.index & 3;
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
    // if (reg != 0)
    // {
    //     DLOG("Read reg%d (idx=%d) = %02X\n", reg, cdrom.index, result);
    // }
    return result;
}

/* ---- CD-ROM event callback (called by scheduler) ---- */
static void CDROM_EventCallback(void)
{
    if (!cdrom.reading)
        return;

    /* Phase 1: Seek completion */
    if (cdrom.seek_pending)
    {
        cdrom.seek_pending = 0;
        cdrom_set_stat(0x22); /* Reading + Motor On */
        /* Schedule first sector delivery */
        Scheduler_ScheduleEvent(SCHED_EVENT_CDROM,
                                global_cycles + CDROM_READ_CYCLES_FAST,
                                CDROM_EventCallback);
        return;
    }

    /* Phase 2: Sector delivery (INT1) */
    if (cdrom.int_flag != 0 || cdrom.has_pending)
    {
        /* Previous INT not acknowledged yet - retry shortly */
        Scheduler_ScheduleEvent(SCHED_EVENT_CDROM,
                                global_cycles + 1000,
                                CDROM_EventCallback);
        return;
    }

    /* Fill data FIFO with sector data */
    if (ISO_IsLoaded())
    {
        /* Convert absolute LBA to file-relative LBA.
         * The BIN/CUE file starts at the data area (Track 1 INDEX 01),
         * which is at absolute sector 150 (2-second pregap).
         * MSF addresses from Setloc are absolute, so we subtract 150. */
        uint32_t file_lba = (cdrom.cur_lba >= PREGAP_LBA) ? (cdrom.cur_lba - PREGAP_LBA) : 0;

        /* Read real sector data from mounted ISO */
        if (ISO_ReadSector(file_lba, cdrom.data_fifo) < 0)
        {
            DLOG("Failed to read sector at LBA %" PRIu32 " (file LBA %" PRIu32 ")\n",
                 cdrom.cur_lba, file_lba);
            memset(cdrom.data_fifo, 0, ISO_SECTOR_SIZE);
        }
    }
    else
    {
        /* No disc image: fill with zeros */
        memset(cdrom.data_fifo, 0, ISO_SECTOR_SIZE);
    }
    cdrom.data_pos = 0;
    cdrom.data_len = ISO_SECTOR_SIZE; /* 2048 bytes normal mode */

    /* Deliver INT1 response */
    {
        uint8_t resp[1];
        resp[0] = cdrom.stat;
        cdrom_queue_response(resp, 1, 1); /* INT1 = data ready */
    }

    /* Advance position */
    cdrom.cur_lba++;

    /* Schedule next sector (~50000 cycles fast approximation) */
    Scheduler_ScheduleEvent(SCHED_EVENT_CDROM,
                            global_cycles + CDROM_READ_CYCLES_FAST,
                            CDROM_EventCallback);
}

/* ---- Pending response callback ---- */
static void CDROM_PendingCallback(void)
{
    if (cdrom.has_pending && cdrom.int_flag == 0)
        cdrom_deliver_pending();
}

/* ---- Schedule a CD-ROM event (public, called from dynarec) ---- */
void CDROM_ScheduleEvent(void)
{
    if (cdrom.reading)
    {
        Scheduler_ScheduleEvent(SCHED_EVENT_CDROM,
                                global_cycles + CDROM_READ_CYCLES_FAST,
                                CDROM_EventCallback);
    }
}

/* ---- Legacy periodic update (retained for deferred delivery + IRQ re-assertion) ---- */
void CDROM_Update(uint32_t cycles)
{
    /* Deliver deferred first response (INT3) after delay */
    if (cdrom.deferred_delay > 0)
    {
        cdrom.deferred_delay -= (int32_t)cycles;
        if (cdrom.deferred_delay <= 0)
        {
            cdrom.deferred_delay = 0;
            if (cdrom.has_deferred)
                cdrom_deliver_deferred();
        }
    }

    /* Delayed I_STAT assertion — models the propagation delay from
     * the CD-ROM controller to the CPU interrupt line.  This gives
     * poll-based code one or more block-execution windows to read
     * int_flag before the ISR fires and clears it. */
    if (cdrom.int_flag != 0)
    {
        if (cdrom.irq_signal_delay > 0)
        {
            cdrom.irq_signal_delay -= (int32_t)cycles;
        }
        else
        {
            SignalInterrupt(2); /* Assert I_STAT bit 2 (CD-ROM) */
        }
    }
}

/* ---- Write CD-ROM register ---- */
void CDROM_Write(uint32_t addr, uint32_t data)
{
    uint32_t reg = addr & 3;
    uint8_t val = data & 0xFF;

    // DLOG("Write reg%d (idx=%d) val=%02X\n", reg, cdrom.index, val);

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
        {
            uint8_t old_flag = cdrom.int_flag;
            cdrom.int_flag &= ~(val & 0x07);
            DLOG("ACK: val=%02X old_flag=%d new_flag=%d has_pending=%d\n",
                 val, old_flag, cdrom.int_flag, cdrom.has_pending);
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
                Scheduler_ScheduleEvent(SCHED_EVENT_CDROM,
                                        global_cycles + 200,
                                        CDROM_PendingCallback);
            }
            break;
        }
        case 2: /* Audio Volume Left→Right */
            break;
        case 3: /* Apply Audio Volume changes */
            break;
        }
        break;
    }
}
