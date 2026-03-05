/*
 * JIT Playground — Test Runner
 *
 * Delegates to per-category test files:
 *   test_alu.c    — ALU, shifts, mul/div, comparisons, HI/LO  (21 tests)
 *   test_memory.c — Load/Store                                  (6 tests)
 *   test_branch.c — Branches, REGIMM                            (7 tests)
 *   test_block.c  — Interactions, cross-block, loops            (14 tests)
 *                                                         Total: 59 tests
 */
#include "playground.h"

void pg_run_all_tests(void)
{
    pg_run_alu_tests();
    pg_run_memory_tests();
    pg_run_branch_tests();
    pg_run_block_tests();
    pg_run_dirty_tests();
}
