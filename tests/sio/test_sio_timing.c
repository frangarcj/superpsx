/*
 * test_sio_timing.c — TDD tests for SIO timing model
 *
 * Tests the SIO layer's immediate-response model for memcard bytes.
 * Matches all 3 reference emulators (PCSX-ReARMed, PCSX-Redux, DuckStation):
 *   - Response data is available immediately after TX write
 *   - Only the IRQ delivery is delayed (SIO_MCD_IRQ_DELAY)
 *   - ack_latch is set immediately in sio_assert_ack (DuckStation InvokeEarly model)
 *   - PadCardIrq polls STAT bit7 (ACKINPUT) to continue card data loop
 *
 * Compile:
 *   cc -I../../include -DTEST_SIO_TIMING -o test_sio_timing test_sio_timing.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ---- Stub: profiler (must come before superpsx.h) ---- */
#define PROF_PUSH(cat) ((void)0)
#define PROF_POP(cat)  ((void)0)

/* ---- Include core headers ---- */
#include "config.h"
#include "scheduler.h"   /* Provides inline Sched_* + sched_active/deadline/callback + global_cycles etc. */

/* ---- Global state that scheduler.h declares extern ---- */
int sched_active[SCHED_EVENT_COUNT];
uint64_t sched_deadline[SCHED_EVENT_COUNT];
sched_callback_t sched_callback[SCHED_EVENT_COUNT];
uint64_t global_cycles = 0;
uint32_t partial_block_cycles = 0;
volatile uint32_t chain_cycles_acc = 0;
int sched_unlimited_speed = 0;
uint64_t sched_cached_earliest = UINT64_MAX;
int sched_earliest_id = -1;
uint64_t hblank_frame_start_cycle = 0;

/* ---- Stub: CPU state ---- */
/* superpsx.h defines `cpu` as R3000CPU and psx_ram as uint8_t* */
#include "superpsx.h"

R3000CPU cpu;
static uint8_t _psx_ram_buf[2 * 1024 * 1024];
uint8_t *psx_ram = _psx_ram_buf;
uint32_t psx_abort_pc = 0;

/* ---- Stub: SignalInterrupt tracking ---- */
static int irq7_fired = 0;
static int irq7_count = 0;
static uint64_t irq7_cycle = 0;

void SignalInterrupt(uint32_t irq) {
    if (irq == 7) {
        irq7_fired = 1;
        irq7_count++;
        irq7_cycle = global_cycles;
    }
}

/* ---- Stub: Joystick ---- */
int Joystick_HasMultitap(int port) { (void)port; return 0; }
int Joystick_IsConnected(int port, int slot) { (void)port; (void)slot; return 0; }
void Joystick_GetPSXDigitalResponse(int port, int slot, uint8_t response[3]) {
    (void)port; (void)slot;
    response[0] = 0xFF; response[1] = 0xFF; response[2] = 0xFF;
}

/* ---- Stub: PSXConfig ---- */
PSXConfig psx_config = {
    .mcd1_path = "/tmp/test_sio_mcd1.mcr",
    .mcd2_path = "/tmp/test_sio_mcd2.mcr",
};

/* ---- Include the actual implementations ---- */
#include "../../src/memorycard.c"
#include "../../src/sio.c"

/* ---- Helper: advance global_cycles and dispatch scheduler ---- */
static void advance_cycles(uint64_t delta)
{
    global_cycles += delta;
    Sched_Tick(global_cycles);
}

/* ---- Helper: drive memcard through header phase on a given port ---- */
static void drive_header_phase_port(int port)
{
    /* SELECT with port in bit 13: 0x1003 for port0, 0x3003 for port1 */
    uint16_t ctrl = 0x1003 | ((port & 1) << 13);
    SIO_Write(0x1F80104A, ctrl);

    /* Byte 1: Access (0x81) */
    SIO_Write(0x1F801040, 0x81);
    advance_cycles(2000);

    /* Byte 2: Read cmd (0x52) */
    SIO_Write(0x1F801040, 0x52);
    advance_cycles(2000);

    /* Byte 3: ID1 */
    SIO_Write(0x1F801040, 0x00);
    advance_cycles(2000);

    /* Byte 4: ID2 */
    SIO_Write(0x1F801040, 0x00);
    advance_cycles(2000);

    /* Byte 5: Addr MSB (sector 0) */
    SIO_Write(0x1F801040, 0x00);
    advance_cycles(2000);

    /* Byte 6: Addr LSB (sector 0) */
    SIO_Write(0x1F801040, 0x00);
    advance_cycles(2000);

    /* Byte 7: ACK1 */
    SIO_Write(0x1F801040, 0x00);
    advance_cycles(2000);

    /* Byte 8: ACK2 */
    SIO_Write(0x1F801040, 0x00);
    advance_cycles(2000);

    /* Byte 9: Confirmed addr MSB */
    SIO_Write(0x1F801040, 0x00);
    advance_cycles(2000);

    /* Byte 10: Confirmed addr LSB */
    SIO_Write(0x1F801040, 0x00);
    advance_cycles(2000);

    /* Now in data phase */
}

/* ---- Helper: drive memcard through header phase (bytes 1-10) ---- */
static void drive_header_phase(void)
{
    drive_header_phase_port(0);
}

/* ---- Helper: deselect (release /SEL) ---- */
static void deselect(void)
{
    SIO_Write(0x1F80104A, 0x0000); /* bit1=0 → deselect */
    advance_cycles(500);
}

/* ---- Test infrastructure ---- */
static int tests_passed = 0;
static int tests_failed = 0;
static int total_checks = 0;

#define CHECK_EQ(actual, expected, msg) do {            \
    total_checks++;                                      \
    unsigned _a = (unsigned)(actual), _e = (unsigned)(expected); \
    if (_a != _e) {                                      \
        printf("  FAIL: %s: got 0x%X, expected 0x%X\n", msg, _a, _e); \
        tests_failed++;                                  \
    }                                                    \
} while(0)

#define CHECK_TRUE(cond, msg) do {   \
    total_checks++;                   \
    if (!(cond)) {                    \
        printf("  FAIL: %s\n", msg); \
        tests_failed++;               \
    }                                 \
} while(0)

#define TEST_BEGIN(name) do {                \
    printf("TEST: %s\n", name);              \
    int _old_failed = tests_failed;          \
    (void)_old_failed;

#define TEST_END(name)                       \
    if (tests_failed == _old_failed) {       \
        tests_passed++;                      \
        printf("  PASS\n");                  \
    }                                        \
} while(0)

/* ---- Helper: full reset of SIO + MCD + scheduler state ---- */
static void full_reset(void)
{
    /* Reset SIO via CTRL bit 6 */
    SIO_Write(0x1F80104A, 0x0040);
    /* Reset scheduler */
    for (int i = 0; i < SCHED_EVENT_COUNT; i++)
        sched_active[i] = 0;
    sched_cached_earliest = UINT64_MAX;
    sched_earliest_id = -1;
    global_cycles = 1000000; /* Start at some nonzero cycle */
    partial_block_cycles = 0;
    irq7_fired = 0;
    irq7_count = 0;
    irq7_cycle = 0;
    memset(&cpu, 0, sizeof(cpu));
    MCD_Reset(0);
    MCD_Reset(1);
}

/* ==================================================================== */
/* TEST 1: Data-phase TX → response available immediately              */
/* ==================================================================== */
static void test_immediate_data_response(void)
{
    TEST_BEGIN("immediate_data_response");
    full_reset();
    drive_header_phase();
    irq7_count = 0;

    /* Write TX for byte 11 (first data-phase byte) */
    SIO_Write(0x1F801040, 0x00);

    /* Response available immediately — matching PCSX-ReARMed/Redux/DS */
    CHECK_EQ(sio_data, 0x4D, "byte11: sio_data='M' (0x4D) immediately");
    CHECK_EQ(sio_tx_pending, 1, "byte11: tx_pending=1 immediately");

    /* IRQ has NOT fired yet (only IRQ is delayed, ack_latch is immediate) */
    CHECK_EQ(irq7_count, 0, "byte11: IRQ not yet fired");

    /* SIO_DATA read returns card response */
    uint32_t val = SIO_Read(0x1F801040);
    CHECK_EQ(val, 0x4D, "byte11: SIO_DATA read = 'M'");

    /* Now advance past IRQ delay */
    advance_cycles(3000);
    CHECK_TRUE(irq7_count > 0, "byte11: IRQ7 fires after delay");

    TEST_END("immediate_data_response");
}

/* ==================================================================== */
/* TEST 2: Memcard ACKINPUT set immediately with TX response            */
/*   DuckStation InvokeEarly model: ACK fires when transfer completes. */
/*   In our instant-response model, ack_latch is set in sio_assert_ack */
/*   so PadCardIrq can poll STAT bit7 to continue the card data loop.  */
/* ==================================================================== */
static void test_memcard_ackinput(void)
{
    TEST_BEGIN("memcard_ackinput");
    full_reset();
    drive_header_phase();
    irq7_count = 0;

    /* Consume stale ack_latch from byte 10's sio_assert_ack */
    SIO_Read(0x1F801044);

    /* Write TX for byte 11 (immediate response + immediate ack_latch) */
    SIO_Write(0x1F801040, 0x00);
    CHECK_EQ(sio_data, 0x4D, "byte11: immediate 'M'");

    /* ACKINPUT should be set immediately (from sio_assert_ack) */
    uint32_t stat = SIO_Read(0x1F801044);
    CHECK_TRUE((stat & 0x80), "STAT bit7=1 after TX (immediate ACKINPUT)");

    /* ack_latch consumed by STAT read — next read should have bit7=0 */
    stat = SIO_Read(0x1F801044);
    CHECK_TRUE(!(stat & 0x80), "STAT bit7=0 on second read (consumed)");

    /* sio_data still holds card response */
    CHECK_EQ(sio_data, 0x4D, "sio_data preserved");

    TEST_END("memcard_ackinput");
}

/* ==================================================================== */
/* TEST 3: Consume-on-read (second SIO_DATA read returns 0xFF) */
/* ==================================================================== */
static void test_consume_on_read(void)
{
    TEST_BEGIN("consume_on_read");
    full_reset();
    drive_header_phase();

    /* Write TX for byte 11 — immediate response */
    SIO_Write(0x1F801040, 0x00);
    CHECK_EQ(sio_data, 0x4D, "before read: sio_data='M'");

    /* First read: returns the real data */
    uint32_t val1 = SIO_Read(0x1F801040);
    CHECK_EQ(val1, 0x4D, "first read: 0x4D ('M')");

    /* Second read: should return 0xFF (consumed) */
    uint32_t val2 = SIO_Read(0x1F801040);
    CHECK_EQ(val2, 0xFF, "second read: 0xFF (consumed)");

    TEST_END("consume_on_read");
}

/* ==================================================================== */
/* TEST 4: IRQ fires AFTER data is available (deferred IRQ + imm ack)  */
/* ==================================================================== */
static void test_deferred_irq(void)
{
    TEST_BEGIN("deferred_irq");
    full_reset();
    drive_header_phase();
    irq7_count = 0;

    uint64_t tx_cycle = global_cycles;

    /* Write TX for byte 11 — immediate response + immediate ack_latch */
    SIO_Write(0x1F801040, 0x00);
    CHECK_EQ(sio_data, 0x4D, "data available immediately");
    CHECK_EQ(sio_ack_latch, 1, "ack_latch set immediately");
    CHECK_EQ(irq7_count, 0, "no IRQ yet at T+0");

    /* Advance 500 cycles — still no IRQ */
    advance_cycles(500);
    CHECK_EQ(irq7_count, 0, "no IRQ at T+500");

    /* Advance until IRQ fires */
    for (int i = 0; i < 5000; i++) {
        advance_cycles(1);
        if (irq7_count > 0) break;
    }

    uint64_t irq_delay = irq7_cycle - tx_cycle;
    printf("  [info] IRQ fired at T+%llu\n", (unsigned long long)irq_delay);
    CHECK_TRUE(irq_delay >= 1400, "IRQ delay >= SIO_MCD_IRQ_DELAY (~1500)");
    CHECK_TRUE(irq7_count > 0, "IRQ7 fired");

    TEST_END("deferred_irq");
}

/* ==================================================================== */
/* TEST 5: Full byte 10-11 boundary simulation                        */
/*   Simulates what the BIOS exception handler does:                   */
/*   1. IRQ fires for byte 10 → exception handler runs                */
/*   2. Header handler reads byte 10 data, writes TX for byte 11      */
/*   3. Pad handler checks STAT → no ACKINPUT → aborts                */
/*   4. RFE → main code runs → transition code                        */
/*   5. IRQ fires for byte 11 → data handler reads card data          */
/* ==================================================================== */
static void test_byte_10_11_boundary(void)
{
    TEST_BEGIN("byte_10_11_boundary (BIOS simulation)");
    full_reset();

    /* SELECT port 0 */
    SIO_Write(0x1F80104A, 0x1003);

    /* Drive bytes 1-9 */
    SIO_Write(0x1F801040, 0x81); advance_cycles(2000);
    SIO_Write(0x1F801040, 0x52); advance_cycles(2000);
    for (int i = 0; i < 4; i++) { SIO_Write(0x1F801040, 0x00); advance_cycles(2000); }
    SIO_Write(0x1F801040, 0x00); advance_cycles(2000); /* ACK1 */
    SIO_Write(0x1F801040, 0x00); advance_cycles(2000); /* ACK2 */
    SIO_Write(0x1F801040, 0x00); advance_cycles(2000); /* conf MSB */

    /* Byte 10: Confirmed addr LSB */
    SIO_Write(0x1F801040, 0x00);
    advance_cycles(2000);

    /* Read byte 10 response */
    uint32_t byte10_val = sio_data;
    printf("  [info] byte10 sio_data = 0x%02X\n", byte10_val);

    /* === BIOS exception handler for byte 10's IRQ === */

    /* 1. Header handler reads STAT (consumes ack_latch from byte 10 IRQ) */
    SIO_Read(0x1F801044);

    /* 2. Header handler reads SIO_DATA for byte 10 */
    uint32_t header_read = SIO_Read(0x1F801040);
    printf("  [info] Header handler reads byte 10: 0x%02X\n", header_read);

    /* 3. Header handler writes TX=0x00 for byte 11 (data phase) */
    irq7_count = 0;
    SIO_Write(0x1F801040, 0x00);

    /* Response is immediately available */
    CHECK_EQ(sio_tx_pending, 1, "byte11 TX: immediate, tx_pending=1");
    CHECK_EQ(sio_data, 0x4D, "byte11 TX: sio_data='M' immediately");

    /* 4. Pad handler checks STAT → ack_latch=1 from byte 11's sio_assert_ack → bit7=1 */
    uint32_t pad_stat = SIO_Read(0x1F801044);
    CHECK_TRUE((pad_stat & 0x80), "pad handler: STAT bit7=1 (ACKINPUT from sio_assert_ack)");

    /* Card data untouched because pad handler didn't read SIO_DATA */
    CHECK_EQ(sio_data, 0x4D, "sio_data preserved after pad STAT check");

    /* 5. Exception handler returns (RFE). BIOS transition code runs. */
    advance_cycles(500);
    printf("  [info] After 500 cycles: irq7=%d\n", irq7_count);

    /* 6. Wait for IRQ to fire for byte 11 */
    advance_cycles(2000);
    CHECK_TRUE(irq7_count > 0, "IRQ7 fired for byte 11");

    /* 7. Data handler reads SIO_DATA → gets 'M' (0x4D) */
    uint32_t data_read = SIO_Read(0x1F801040);
    CHECK_EQ(data_read, 0x4D, "data handler reads 'M' (0x4D)");

    TEST_END("byte_10_11_boundary (BIOS simulation)");
}

/* ==================================================================== */
/* TEST 6: ack_latch for memcard (DuckStation InvokeEarly model)        */
/*   ACKINPUT (bit 7) IS set immediately via sio_assert_ack().         */
/*   Consumed on first STAT read (latch model).                        */
/* ==================================================================== */
static void test_ack_latch_memcard(void)
{
    TEST_BEGIN("ack_latch_memcard");
    full_reset();
    drive_header_phase();

    /* Write TX for byte 11 → sio_assert_ack sets ack_latch=1 */
    SIO_Write(0x1F801040, 0x00);
    CHECK_EQ(sio_ack_latch, 1, "ack_latch=1 after TX (immediate)");

    /* STAT read consumes ack_latch → bit7=1, then ack_latch=0 */
    uint32_t stat = SIO_Read(0x1F801044);
    CHECK_TRUE((stat & 0x80), "STAT bit7=1 (ACKINPUT from sio_assert_ack)");
    CHECK_EQ(sio_ack_latch, 0, "ack_latch=0 after STAT read (consumed)");

    /* Second STAT read: bit7=0 */
    stat = SIO_Read(0x1F801044);
    CHECK_TRUE(!(stat & 0x80), "STAT bit7=0 on second read");

    TEST_END("ack_latch_memcard");
}

/* ==================================================================== */
/* TEST 7: Multiple data-phase bytes in sequence                       */
/* ==================================================================== */
static void test_sequential_data_bytes(void)
{
    TEST_BEGIN("sequential_data_bytes");
    full_reset();
    drive_header_phase();

    /* Read bytes 11-13 (first 3 data bytes) */
    const uint8_t expected[] = { 0x4D, 0x43, 0x00 }; /* 'M', 'C', 0x00 */

    for (int i = 0; i < 3; i++) {
        SIO_Write(0x1F801040, 0x00);
        /* Response is immediate */
        CHECK_EQ(sio_tx_pending, 1, "data byte immediate");

        /* Read data */
        uint32_t val = SIO_Read(0x1F801040);
        char msg[64];
        snprintf(msg, sizeof(msg), "data byte %d = 0x%02X", i, expected[i]);
        CHECK_EQ(val, expected[i], msg);

        /* Advance past IRQ for this byte */
        advance_cycles(5000);
    }

    TEST_END("sequential_data_bytes");
}

/* ==================================================================== */
/* TEST 8: [RED] Immediate response — all 3 reference emulators agree  */
/*                                                                      */
/* PCSX-ReARMed: response in buf[] immediately                          */
/* PCSX-Redux:   response in FIFO immediately                           */
/* DuckStation:  response after Transfer event (~768cy), forced early    */
/*               by InvokeEarly on SIO_DATA read                        */
/*                                                                      */
/* Consensus: after writing TX for a data-phase byte, the response      */
/* must be available IMMEDIATELY (or at most on first SIO_DATA read).   */
/* Our current model defers for 1400cy (sio_data=0xFF until callback).  */
/* This test will FAIL (RED) until we fix the deferred model.           */
/* ==================================================================== */
static void test_immediate_response(void)
{
    TEST_BEGIN("immediate_response");
    full_reset();
    drive_header_phase();
    irq7_count = 0;

    /* Write TX for byte 11 (first data-phase byte) */
    SIO_Write(0x1F801040, 0x00);

    /* === KEY ASSERTION: response must be available IMMEDIATELY === */
    /* All 3 reference emulators make data available right after write.
     * Our current model returns 0xFF (deferred) — this should FAIL. */
    /* tx_pending should be 1 (data is ready for reading) */
    CHECK_EQ(sio_tx_pending, 1, "immediate: tx_pending=1 after TX");

    uint32_t data_val = SIO_Read(0x1F801040);
    CHECK_EQ(data_val, 0x4D, "immediate: SIO_DATA = 'M' (0x4D) right after TX");

    /* IRQ should NOT have fired yet (deferred, not immediate) */
    CHECK_EQ(irq7_count, 0, "immediate: IRQ not fired yet");

    /* Now advance past IRQ delay — IRQ should fire */
    advance_cycles(3000);
    CHECK_TRUE(irq7_count > 0, "immediate: IRQ fires after delay");

    TEST_END("immediate_response");
}

/* ==================================================================== */
/* TEST 9: [RED] Second data-phase byte also immediate                  */
/* ==================================================================== */
static void test_immediate_sequential(void)
{
    TEST_BEGIN("immediate_sequential");
    full_reset();
    drive_header_phase();
    irq7_count = 0;

    /* Byte 11: tx=0x00, expect 'M' (0x4D) immediately */
    SIO_Write(0x1F801040, 0x00);
    uint32_t v1 = SIO_Read(0x1F801040);
    CHECK_EQ(v1, 0x4D, "byte11: immediate 'M'");

    /* Advance past IRQ for byte 11 */
    advance_cycles(3000);
    irq7_count = 0;

    /* Byte 12: tx=0x00, expect 'C' (0x43) immediately */
    SIO_Write(0x1F801040, 0x00);
    uint32_t v2 = SIO_Read(0x1F801040);
    CHECK_EQ(v2, 0x43, "byte12: immediate 'C'");

    CHECK_EQ(irq7_count, 0, "byte12: IRQ not yet (deferred)");
    advance_cycles(3000);
    CHECK_TRUE(irq7_count > 0, "byte12: IRQ fires after delay");

    TEST_END("immediate_sequential");
}

/* ==================================================================== */
/* TEST 10: Both memcard slots return identical data for same file      */
/*   Regression: BIOS showed fake block on card 1 but not card 2       */
/*   even though both .mcd files were identical copies.                 */
/* ==================================================================== */
static void test_dual_memcard_identical(void)
{
    TEST_BEGIN("dual_memcard_identical");
    full_reset();

    /* --- Read 128 data bytes from slot 0 (port 0) --- */
    uint8_t slot0_data[128];
    drive_header_phase_port(0);
    for (int i = 0; i < 128; i++) {
        SIO_Write(0x1F801040, 0x00);
        slot0_data[i] = (uint8_t)SIO_Read(0x1F801040);
        advance_cycles(2000);
    }
    deselect();

    /* --- Read 128 data bytes from slot 1 (port 1) --- */
    uint8_t slot1_data[128];
    drive_header_phase_port(1);
    for (int i = 0; i < 128; i++) {
        SIO_Write(0x1F801040, 0x00);
        slot1_data[i] = (uint8_t)SIO_Read(0x1F801040);
        advance_cycles(2000);
    }
    deselect();

    /* --- Verify both slots returned the same data --- */
    CHECK_EQ(slot0_data[0], 0x4D, "slot0 byte0 = 'M'");
    CHECK_EQ(slot0_data[1], 0x43, "slot0 byte1 = 'C'");
    CHECK_EQ(slot1_data[0], 0x4D, "slot1 byte0 = 'M'");
    CHECK_EQ(slot1_data[1], 0x43, "slot1 byte1 = 'C'");

    int differ = 0;
    for (int i = 0; i < 128; i++) {
        if (slot0_data[i] != slot1_data[i]) {
            if (differ < 5)
                printf("  DIFF: byte[%d] slot0=0x%02X slot1=0x%02X\n",
                       i, slot0_data[i], slot1_data[i]);
            differ++;
        }
    }
    CHECK_EQ(differ, 0, "all 128 data bytes identical across slots");

    TEST_END("dual_memcard_identical");
}

/* ==================================================================== */
/* TEST 11: Port switch with sel=1 resets card state                    */
/*   Regression: BIOS switches port without deselecting; if SIO state  */
/*   isn't reset on port change, old card's exchange contaminates new.  */
/* ==================================================================== */
static void test_port_switch_resets_state(void)
{
    TEST_BEGIN("port_switch_resets_state");
    full_reset();

    /* Start exchange on port 0 */
    SIO_Write(0x1F80104A, 0x1003); /* port 0, sel=1 */
    SIO_Write(0x1F801040, 0x81);   /* byte0: card access */
    advance_cycles(2000);
    SIO_Write(0x1F801040, 0x52);   /* byte1: READ cmd */
    advance_cycles(2000);
    SIO_Write(0x1F801040, 0x00);   /* byte2: ID1 */
    advance_cycles(2000);

    /* Card 0 should be at ID2 (phase 3) now */

    /* Switch to port 1 WITHOUT deselecting (just change port bit) */
    SIO_Write(0x1F80104A, 0x3003); /* port 1, sel=1 */

    /* Card 1 should be at IDLE (fresh), not contaminated by port 0 */
    CHECK_EQ(MCD_IsIdle(1), 1, "port1 card is IDLE after port switch");

    /* Start fresh exchange on port 1 */
    SIO_Write(0x1F801040, 0x81);   /* byte0: card access */
    advance_cycles(2000);
    SIO_Write(0x1F801040, 0x52);   /* byte1: READ cmd */
    advance_cycles(2000);

    /* PORT 1 should get proper FLAG response (0x08), not garbage */
    uint32_t flag = SIO_Read(0x1F801040);
    CHECK_EQ(flag, 0x08, "port1 FLAG = 0x08 after port switch");

    deselect();

    /* Card 0 should also have been reset when port switched */
    CHECK_EQ(MCD_IsIdle(0), 1, "port0 card reset after port switch");

    TEST_END("port_switch_resets_state");
}

int main(void)
{
    printf("=== SIO Timing TDD Tests ===\n\n");

    /* Clean temp files */
    remove("/tmp/test_sio_mcd1.mcr");
    remove("/tmp/test_sio_mcd2.mcr");

    /* Init memory cards */
    MCD_Init();

    test_immediate_data_response();
    test_memcard_ackinput();
    test_consume_on_read();
    test_deferred_irq();
    test_byte_10_11_boundary();
    test_ack_latch_memcard();
    test_sequential_data_bytes();
    test_immediate_response();
    test_immediate_sequential();
    test_dual_memcard_identical();
    test_port_switch_resets_state();

    printf("\n=== Results: %d passed, %d failed (%d checks) ===\n",
        tests_passed, tests_failed, total_checks);

    return tests_failed ? 1 : 0;
}
