/**
 * vu0_micro_psp.c — VU0 micro stubs for PSP (no VU0 hardware)
 *
 * PSP has VFPU instead of VU0. These stubs satisfy the VU0 interface.
 */
#include "superpsx.h"

void vu0_micro_init(void) {
    /* No VU0 on PSP — nothing to initialize */
}

void vu0_micro_prepare_matrix(R3000CPU *cpu, uint32_t mx_cv) {
    (void)cpu; (void)mx_cv;
    /* No VU0 on PSP — matrix preparation is a no-op.
     * GTE ops fall back to software or VFPU paths. */
}
