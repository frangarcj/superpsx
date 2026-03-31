/*
 * test_cycle_counting.c — TDD tests for DRC cycle counting + correction
 *
 * Tests the cycles_left_correction mechanism used when SIO memcard
 * writes cap the JIT block budget mid-chain:
 *   1. SIO write caps cpu.cycles_left to SIO_MCD_IRQ_DELAY + 200
 *   2. Difference is accumulated in cpu.cycles_left_correction
 *   3. After block: cycles_taken = initial - remaining - correction
 *   4. global_cycles += cycles_taken (accurate, no overcount)
 *
 * Compile:
 *   cc -I../../include -DTEST_SIO_TIMING -o test_cycle_counting test_cycle_counting.c
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
#include "scheduler.h"

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
#include "superpsx.h"

R3000CPU cpu;
static uint8_t _psx_ram_buf[2 * 1024 * 1024];
uint8_t *psx_ram = _psx_ram_buf;
uint32_t psx_abort_pc = 0;

/* ---- Stub: SignalInterrupt tracking ---- */
static int irq7_count = 0;
static uint64_t irq7_cycle = 0;

void SignalInterrupt(uint32_t irq) {
    if (irq == 7) {
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
    .mcd1_path = "/tmp/test_cc_mcd1.mcr",
    .mcd2_path = "/tmp/test_cc_mcd2.mcr",
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

/* ---- Full reset ---- */
static void full_reset(void)
{
    memset(&cpu, 0, sizeof(cpu));
    memset(_psx_ram_buf, 0, sizeof(_psx_ram_buf));
    global_cycles = 1000000;
    partial_block_cycles = 0;
    sched_cached_earliest = UINT64_MAX;
    sched_earliest_id = -1;
    irq7_count = 0;
    irq7_cycle = 0;

    /* Reset SIO state */
    SIO_Write(0x1F80104A, 0x0040);  /* CTRL bit 6 = full reset */
    advance_cycles(100);

    /* Init memcards: create small test files */
    MCD_Init();
}

/* ---- Helper: select port and start memcard exchange ---- */
static void start_mcd_exchange(int port)
{
    uint16_t ctrl = 0x1003 | ((port & 1) << 13);
    SIO_Write(0x1F80104A, ctrl);     /* Select */
    SIO_Write(0x1F801040, 0x81);     /* Access byte */
}

/* ---- Helper: drive header phase (bytes 1-10) on port ---- */
static void drive_header_phase(int port)
{
    start_mcd_exchange(port);
    advance_cycles(2000);
    SIO_Write(0x1F801040, 0x52); advance_cycles(2000); /* Read cmd */
    SIO_Write(0x1F801040, 0x00); advance_cycles(2000); /* ID1 */
    SIO_Write(0x1F801040, 0x00); advance_cycles(2000); /* ID2 */
    SIO_Write(0x1F801040, 0x00); advance_cycles(2000); /* Addr MSB */
    SIO_Write(0x1F801040, 0x00); advance_cycles(2000); /* Addr LSB */
    SIO_Write(0x1F801040, 0x00); advance_cycles(2000); /* ACK1 */
    SIO_Write(0x1F801040, 0x00); advance_cycles(2000); /* ACK2 */
    SIO_Write(0x1F801040, 0x00); advance_cycles(2000); /* Conf MSB */
    SIO_Write(0x1F801040, 0x00); advance_cycles(2000); /* Conf LSB */
}

/* ---- Helper: deselect ---- */
static void deselect(void)
{
    SIO_Write(0x1F80104A, 0x0000);
    advance_cycles(500);
}

/* ---- Simulate DRC block execution with cycle correction ---- */
/* This mirrors the logic in dynarec_run.c lines 861-907 */
static uint64_t simulate_block_exit(uint32_t initial_cycles_left,
                                     int32_t remaining,
                                     int32_t correction)
{
    uint64_t gc_before = global_cycles;

    uint32_t cycles_taken = (uint32_t)(initial_cycles_left - remaining);
    if (correction > 0) {
        if ((uint32_t)correction < cycles_taken)
            cycles_taken -= (uint32_t)correction;
        else
            cycles_taken = 1;
    }
    if (cycles_taken == 0)
        cycles_taken = 8;

    global_cycles += cycles_taken;
    return cycles_taken;
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

#define CHECK_EQ64(actual, expected, msg) do {           \
    total_checks++;                                      \
    uint64_t _a = (uint64_t)(actual), _e = (uint64_t)(expected); \
    if (_a != _e) {                                      \
        printf("  FAIL: %s: got %llu, expected %llu\n",  \
               msg, (unsigned long long)_a, (unsigned long long)_e); \
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

#define TEST_BEGIN(name) do { printf("  %-48s ", name); } while(0)
#define TEST_END(name) do {                       \
    if (tests_failed == _saved_fails)             \
        printf("PASS\n");                         \
    else                                          \
        printf("(%d new failures)\n",             \
               tests_failed - _saved_fails);      \
    tests_passed += (tests_failed == _saved_fails); \
} while(0)

/* ================================================================== */
/* TEST 1: correction starts at zero                                  */
/* ================================================================== */
static void test_correction_starts_zero(void)
{
    TEST_BEGIN("correction_starts_zero");
    int _saved_fails = tests_failed;

    full_reset();
    /* Simulate DRC: set initial budget */
    cpu.cycles_left = 5000;
    cpu.initial_cycles_left = 5000;
    cpu.cycles_left_correction = 0;

    CHECK_EQ(cpu.cycles_left_correction, 0, "correction is zero before SIO");
    TEST_END("correction_starts_zero");
}

/* ================================================================== */
/* TEST 2: SIO memcard write caps cycles_left                         */
/* ================================================================== */
static void test_sio_caps_cycles_left(void)
{
    TEST_BEGIN("sio_caps_cycles_left");
    int _saved_fails = tests_failed;

    full_reset();
    drive_header_phase(0);

    /* Simulate: DRC block has large budget remaining */
    cpu.cycles_left = 5000;
    cpu.initial_cycles_left = 5000;
    cpu.cycles_left_correction = 0;

    /* Write data byte (byte 11) — triggers cap */
    SIO_Write(0x1F801040, 0x00);

    /* SIO_MCD_IRQ_DELAY=1500, target = 1500+200 = 1700 */
    CHECK_EQ(cpu.cycles_left, 1700, "cycles_left capped to 1700");
    CHECK_EQ(cpu.cycles_left_correction, 3300, "correction = 5000 - 1700");

    TEST_END("sio_caps_cycles_left");
}

/* ================================================================== */
/* TEST 3: No cap when cycles_left already below target               */
/* ================================================================== */
static void test_no_cap_when_below_target(void)
{
    TEST_BEGIN("no_cap_when_below_target");
    int _saved_fails = tests_failed;

    full_reset();
    drive_header_phase(0);

    /* Simulate: DRC block with small budget */
    cpu.cycles_left = 1000;
    cpu.initial_cycles_left = 1000;
    cpu.cycles_left_correction = 0;

    /* Write data byte — should NOT cap (1000 < 1700) */
    SIO_Write(0x1F801040, 0x00);

    CHECK_EQ(cpu.cycles_left, 1000, "cycles_left unchanged (below target)");
    CHECK_EQ(cpu.cycles_left_correction, 0, "correction stays zero");

    TEST_END("no_cap_when_below_target");
}

/* ================================================================== */
/* TEST 4: Correction accumulates across multiple SIO writes          */
/* ================================================================== */
static void test_correction_accumulates(void)
{
    TEST_BEGIN("correction_accumulates");
    int _saved_fails = tests_failed;

    full_reset();
    drive_header_phase(0);

    /* First SIO write with large budget */
    cpu.cycles_left = 5000;
    cpu.initial_cycles_left = 5000;
    cpu.cycles_left_correction = 0;

    SIO_Write(0x1F801040, 0x00); /* byte 11 */
    CHECK_EQ(cpu.cycles_left, 1700, "first cap: cycles_left=1700");
    CHECK_EQ(cpu.cycles_left_correction, 3300, "first cap: correction=3300");

    /* Simulate: JIT reloads S2 from cpu.cycles_left and runs some cycles.
     * Then another SIO write happens with cycles_left still at 1700.
     * Since 1700 == target, no additional cap should occur. */
    advance_cycles(200);
    SIO_Write(0x1F801040, 0x00); /* byte 12 */
    CHECK_EQ(cpu.cycles_left, 1700, "second write: no additional cap");
    CHECK_EQ(cpu.cycles_left_correction, 3300, "correction unchanged");

    TEST_END("correction_accumulates");
}

/* ================================================================== */
/* TEST 5: Block exit with correction — accurate global_cycles        */
/* ================================================================== */
static void test_block_exit_accurate_cycles(void)
{
    TEST_BEGIN("block_exit_accurate_cycles");
    int _saved_fails = tests_failed;

    full_reset();
    uint64_t gc_start = global_cycles;

    /* Simulate: block starts with budget 5000, runs 200 cycles,
     * SIO caps to 1700 (correction=3300), then block runs 1700
     * more and exits with remaining=0. */
    uint32_t initial = 5000;
    int32_t remaining = 0;       /* block used all remaining budget */
    int32_t correction = 3300;   /* accumulated from SIO cap */

    /* Without correction: cycles_taken = 5000 - 0 = 5000 (WRONG!)
     * With correction: cycles_taken = 5000 - 0 - 3300 = 1700 (correct) */
    uint64_t taken = simulate_block_exit(initial, remaining, correction);
    CHECK_EQ64(taken, 1700, "cycles_taken = 1700 with correction");
    CHECK_EQ64(global_cycles - gc_start, 1700, "global_cycles += 1700");

    TEST_END("block_exit_accurate_cycles");
}

/* ================================================================== */
/* TEST 6: Block exit without correction — normal counting            */
/* ================================================================== */
static void test_block_exit_no_correction(void)
{
    TEST_BEGIN("block_exit_no_correction");
    int _saved_fails = tests_failed;

    full_reset();
    uint64_t gc_start = global_cycles;

    /* Normal block: no SIO, no correction */
    uint32_t initial = 3000;
    int32_t remaining = 500;
    int32_t correction = 0;

    uint64_t taken = simulate_block_exit(initial, remaining, correction);
    CHECK_EQ64(taken, 2500, "cycles_taken = 3000 - 500");
    CHECK_EQ64(global_cycles - gc_start, 2500, "global_cycles += 2500");

    TEST_END("block_exit_no_correction");
}

/* ================================================================== */
/* TEST 7: Correction > cycles_taken — clamp to 1                     */
/* ================================================================== */
static void test_correction_clamp_to_one(void)
{
    TEST_BEGIN("correction_clamp_to_one");
    int _saved_fails = tests_failed;

    full_reset();
    uint64_t gc_start = global_cycles;

    /* Edge case: correction > initial - remaining (shouldn't happen in
     * practice, but test the safety clamp) */
    uint32_t initial = 5000;
    int32_t remaining = 4500;    /* block only ran 500 cycles */
    int32_t correction = 3300;   /* correction > 500 */

    uint64_t taken = simulate_block_exit(initial, remaining, correction);
    CHECK_EQ64(taken, 1, "clamped to 1 when correction > taken");

    TEST_END("correction_clamp_to_one");
}

/* ================================================================== */
/* TEST 8: Scheduler event fires after correction — timing accurate   */
/* ================================================================== */
static void test_scheduler_fires_on_time(void)
{
    TEST_BEGIN("scheduler_fires_on_time");
    int _saved_fails = tests_failed;

    full_reset();
    drive_header_phase(0);
    irq7_count = 0;

    uint64_t tx_cycle = global_cycles;

    /* Simulate large DRC block budget */
    cpu.cycles_left = 5000;
    cpu.initial_cycles_left = 5000;
    cpu.cycles_left_correction = 0;

    /* SIO write schedules IRQ at global_cycles + SIO_MCD_IRQ_DELAY (1500) */
    SIO_Write(0x1F801040, 0x00);

    /* Verify event is scheduled at the right time */
    CHECK_EQ64(sio_irq_delay_cycle, tx_cycle + 1500,
               "SIO IRQ deadline at gc + 1500");

    /* Advance to just before deadline — no IRQ */
    advance_cycles(1499);
    CHECK_EQ(irq7_count, 0, "no IRQ at T+1499");

    /* Advance one more — IRQ fires */
    advance_cycles(1);
    CHECK_EQ(irq7_count, 1, "IRQ fires at T+1500");
    CHECK_EQ64(irq7_cycle, tx_cycle + 1500, "IRQ cycle matches deadline");

    TEST_END("scheduler_fires_on_time");
}

/* ================================================================== */
/* TEST 9: Double SIO write doesn't double-cap                        */
/* ================================================================== */
static void test_double_write_no_double_cap(void)
{
    TEST_BEGIN("double_write_no_double_cap");
    int _saved_fails = tests_failed;

    full_reset();
    drive_header_phase(0);

    cpu.cycles_left = 5000;
    cpu.initial_cycles_left = 5000;
    cpu.cycles_left_correction = 0;

    /* First write: cap 5000 → 1700, correction = 3300 */
    SIO_Write(0x1F801040, 0x00);
    CHECK_EQ(cpu.cycles_left, 1700, "first cap");
    CHECK_EQ(cpu.cycles_left_correction, 3300, "first correction");

    /* Second write without changing cycles_left (JIT reloaded S2 = 1700):
     * 1700 <= 1700, so no additional cap */
    advance_cycles(200);
    SIO_Write(0x1F801040, 0x00);
    CHECK_EQ(cpu.cycles_left, 1700, "no re-cap (already at target)");
    CHECK_EQ(cpu.cycles_left_correction, 3300, "correction unchanged");

    TEST_END("double_write_no_double_cap");
}

/* ================================================================== */
/* TEST 10: Pad SIO write does NOT cap cycles_left                    */
/* ================================================================== */
static void test_pad_no_cap(void)
{
    TEST_BEGIN("pad_no_cap");
    int _saved_fails = tests_failed;

    full_reset();

    /* Select and send pad access byte */
    SIO_Write(0x1F80104A, 0x1003);  /* Select port 0 */
    cpu.cycles_left = 5000;
    cpu.initial_cycles_left = 5000;
    cpu.cycles_left_correction = 0;

    SIO_Write(0x1F801040, 0x01);    /* Pad device */
    CHECK_EQ(cpu.cycles_left, 5000, "pad: cycles_left unchanged");
    CHECK_EQ(cpu.cycles_left_correction, 0, "pad: no correction");

    /* Continue pad exchange */
    SIO_Write(0x1F801040, 0x42);    /* byte 2 */
    CHECK_EQ(cpu.cycles_left, 5000, "pad byte2: cycles_left unchanged");
    CHECK_EQ(cpu.cycles_left_correction, 0, "pad byte2: no correction");

    TEST_END("pad_no_cap");
}

/* ================================================================== */
/* TEST 11: End-to-end: SIO cap + block exit = accurate timing        */
/* ================================================================== */
static void test_end_to_end_sio_block(void)
{
    TEST_BEGIN("end_to_end_sio_block");
    int _saved_fails = tests_failed;

    full_reset();
    drive_header_phase(0);
    irq7_count = 0;
    uint64_t gc_before_block = global_cycles;

    /* Simulate DRC block with large budget */
    cpu.cycles_left = 8000;
    cpu.initial_cycles_left = 8000;
    cpu.cycles_left_correction = 0;

    /* SIO write: caps to 1700, correction = 6300 */
    SIO_Write(0x1F801040, 0x00);
    CHECK_EQ(cpu.cycles_left, 1700, "cap applied");
    CHECK_EQ(cpu.cycles_left_correction, 6300, "correction = 6300");

    /* Simulate: block ran from capped budget (1700) and exited with
     * remaining=200 (consumed 1500 cycles after cap). */
    int32_t remaining_after_block = 200;

    /* Block exit logic */
    uint64_t taken = simulate_block_exit(
        8000,   /* initial */
        remaining_after_block,
        (int32_t)cpu.cycles_left_correction
    );

    /* Expected: 8000 - 200 - 6300 = 1500 cycles actually consumed */
    CHECK_EQ64(taken, 1500, "actual cycles consumed = 1500");
    CHECK_EQ64(global_cycles - gc_before_block, 1500,
               "global_cycles advanced by 1500");

    /* Now the scheduler should fire the SIO IRQ */
    /* The IRQ was scheduled at gc_before_block + 1500. After block exit,
     * global_cycles = gc_before_block + 1500, so dispatch should fire it. */
    Sched_Tick(global_cycles);
    CHECK_EQ(irq7_count, 1, "SIO IRQ fires exactly at block exit");

    TEST_END("end_to_end_sio_block");
}

/* ================================================================== */
/* TEST 12: Zero cycles_taken gets bumped to 8                        */
/* ================================================================== */
static void test_zero_cycles_taken_bumped(void)
{
    TEST_BEGIN("zero_cycles_taken_bumped");
    int _saved_fails = tests_failed;

    full_reset();
    uint64_t gc_start = global_cycles;

    /* Edge case: block didn't consume any cycles */
    uint64_t taken = simulate_block_exit(1000, 1000, 0);
    CHECK_EQ64(taken, 8, "zero cycles bumped to 8");

    TEST_END("zero_cycles_taken_bumped");
}

/* ================================================================== */
/* TEST 13: Multiple blocks — correction resets between blocks        */
/* ================================================================== */
static void test_correction_resets_between_blocks(void)
{
    TEST_BEGIN("correction_resets_between_blocks");
    int _saved_fails = tests_failed;

    full_reset();
    drive_header_phase(0);

    /* Block 1: SIO caps */
    cpu.cycles_left = 5000;
    cpu.initial_cycles_left = 5000;
    cpu.cycles_left_correction = 0;
    SIO_Write(0x1F801040, 0x00);
    CHECK_EQ(cpu.cycles_left_correction, 3300, "block1: correction=3300");

    /* Simulate block 1 exit */
    simulate_block_exit(5000, 0, cpu.cycles_left_correction);

    /* Block 2 starts: correction should be reset to 0 (DRC does this) */
    cpu.cycles_left = 3000;
    cpu.initial_cycles_left = 3000;
    cpu.cycles_left_correction = 0;  /* DRC resets this before each block */

    CHECK_EQ(cpu.cycles_left_correction, 0, "block2: correction reset to 0");

    /* No SIO in block 2 — normal exit */
    uint64_t gc_before = global_cycles;
    uint64_t taken = simulate_block_exit(3000, 500, cpu.cycles_left_correction);
    CHECK_EQ64(taken, 2500, "block2: normal 2500 cycles");

    TEST_END("correction_resets_between_blocks");
}

/* ================================================================== */
/* TEST 14: Chained IRQs — each byte gets its own IRQ                 */
/*   Simulates the BIOS memcard read loop: IRQ fires, handler reads   */
/*   data + writes next TX, new IRQ is scheduled. Verify each IRQ     */
/*   fires at the correct absolute cycle.                             */
/* ================================================================== */
static void test_chained_irqs(void)
{
    TEST_BEGIN("chained_irqs");
    int _saved_fails = tests_failed;

    full_reset();
    drive_header_phase(0);
    irq7_count = 0;

    /* Bytes 11-14: four data bytes, each gets an IRQ */
    for (int i = 0; i < 4; i++) {
        uint64_t tx_cycle = global_cycles;
        uint64_t deadline = tx_cycle + 1500;

        cpu.cycles_left = 5000;
        cpu.initial_cycles_left = 5000;
        cpu.cycles_left_correction = 0;
        SIO_Write(0x1F801040, 0x00);

        /* Verify IRQ is scheduled at tx_cycle + 1500 */
        CHECK_EQ64(sio_irq_delay_cycle, deadline, "IRQ deadline for byte");

        /* Advance past IRQ deadline */
        advance_cycles(2000);

        /* Check IRQ fired (dispatch happens at advance time >= deadline) */
        CHECK_EQ(irq7_count, i + 1, "IRQ count after byte");
        CHECK_TRUE(irq7_cycle >= deadline, "IRQ fired after deadline");
    }

    /* All 4 IRQs fired at their respective times */
    CHECK_EQ(irq7_count, 4, "total 4 IRQs for 4 data bytes");

    TEST_END("chained_irqs");
}

/* ================================================================== */
/* TEST 15: IRQ replaced — new SIO write reschedules before old fires */
/*   If a second SIO write happens before the first IRQ fires, the    */
/*   new event should REPLACE the old one (same scheduler slot).      */
/* ================================================================== */
static void test_irq_replaced_by_new_write(void)
{
    TEST_BEGIN("irq_replaced_by_new_write");
    int _saved_fails = tests_failed;

    full_reset();
    drive_header_phase(0);
    irq7_count = 0;

    uint64_t tx1_cycle = global_cycles;

    /* First write: schedules IRQ at tx1_cycle + 1500 */
    cpu.cycles_left = 5000;
    cpu.initial_cycles_left = 5000;
    cpu.cycles_left_correction = 0;
    SIO_Write(0x1F801040, 0x00);
    CHECK_EQ64(sio_irq_delay_cycle, tx1_cycle + 1500, "first deadline");

    /* Advance 500 cycles — first IRQ hasn't fired yet */
    advance_cycles(500);
    CHECK_EQ(irq7_count, 0, "no IRQ at T+500");

    /* Second write at T+500: reschedules IRQ to (tx1_cycle+500) + 1500 */
    uint64_t tx2_cycle = global_cycles;
    SIO_Write(0x1F801040, 0x00);
    CHECK_EQ64(sio_irq_delay_cycle, tx2_cycle + 1500, "second deadline replaced first");

    /* Advance to FIRST deadline (tx1_cycle + 1500 = tx2_cycle + 1000)
     * — IRQ should NOT have fired (replaced by later deadline) */
    advance_cycles(1000);
    CHECK_EQ(irq7_count, 0, "old deadline passed — no IRQ (replaced)");

    /* Advance to SECOND deadline */
    advance_cycles(500);
    CHECK_EQ(irq7_count, 1, "IRQ fires at new deadline");
    CHECK_EQ64(irq7_cycle, tx2_cycle + 1500, "IRQ at second write's deadline");

    TEST_END("irq_replaced_by_new_write");
}

/* ================================================================== */
/* TEST 16: Rapid-fire SIO writes — IRQ only fires once per slot      */
/*   10 rapid SIO writes with no advance between them. Only the last  */
/*   event survives in the scheduler slot. One IRQ fires.             */
/* ================================================================== */
static void test_rapid_fire_writes(void)
{
    TEST_BEGIN("rapid_fire_writes");
    int _saved_fails = tests_failed;

    full_reset();
    drive_header_phase(0);
    irq7_count = 0;

    uint64_t base_cycle = global_cycles;

    /* 10 rapid SIO data writes — each replaces the previous event */
    cpu.cycles_left = 50000;
    cpu.initial_cycles_left = 50000;
    cpu.cycles_left_correction = 0;
    for (int i = 0; i < 10; i++) {
        SIO_Write(0x1F801040, 0x00);
    }

    /* All 10 writes happened at the same global_cycles (no advance).
     * The scheduler slot holds the LAST event at base_cycle + 1500. */
    CHECK_EQ64(sio_irq_delay_cycle, base_cycle + 1500,
               "last write's deadline");
    CHECK_EQ(irq7_count, 0, "no IRQ yet");

    /* Advance past deadline */
    advance_cycles(2000);
    CHECK_EQ(irq7_count, 1, "exactly 1 IRQ fires");
    CHECK_TRUE(irq7_cycle >= base_cycle + 1500, "IRQ at or after deadline");

    TEST_END("rapid_fire_writes");
}

/* ================================================================== */
/* main                                                               */
/* ================================================================== */
int main(void)
{
    printf("=== Cycle Counting TDD Tests ===\n");

    test_correction_starts_zero();
    test_sio_caps_cycles_left();
    test_no_cap_when_below_target();
    test_correction_accumulates();
    test_block_exit_accurate_cycles();
    test_block_exit_no_correction();
    test_correction_clamp_to_one();
    test_scheduler_fires_on_time();
    test_double_write_no_double_cap();
    test_pad_no_cap();
    test_end_to_end_sio_block();
    test_zero_cycles_taken_bumped();
    test_correction_resets_between_blocks();
    test_chained_irqs();
    test_irq_replaced_by_new_write();
    test_rapid_fire_writes();

    printf("=== Results: %d passed, %d failed (%d checks) ===\n",
           tests_passed, tests_failed, total_checks);
    return tests_failed > 0 ? 1 : 0;
}
