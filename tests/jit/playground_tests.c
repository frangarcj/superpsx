/*
 * JIT Playground — Test Runner
 *
 * Delegates to per-category test files:
 *   test_alu.c    — ALU, shifts, mul/div, comparisons, HI/LO  (21 tests)
 *   test_memory.c — Load/Store                                  (6 tests)
 *   test_branch.c — Branches, REGIMM                            (7 tests)
 *   test_block.c  — Interactions, cross-block, loops            (14 tests)
 *   test_sio.c    — SIO/scheduler integration                   (6 tests)
 *                                                         Total: 65+ tests
 */
#include "playground.h"

/* Forward declaration for SIO tests */
void pg_run_sio_tests(void);

void pg_run_all_tests(void)
{
    pg_run_alu_tests();
    pg_run_memory_tests();
    pg_run_branch_tests();
    pg_run_block_tests();
    pg_run_dirty_tests();
    pg_run_gte_tests();
    pg_run_sio_tests();
#ifdef PLATFORM_PS2
    pg_run_vu0_micro_tests();
    pg_run_gte_compare_tests();
#endif

    /* Expansion ratio report (compile-only, no pass/fail) */
    pg_run_expansion_tests();
}
