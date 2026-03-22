#ifndef INTERPRETER_H
#define INTERPRETER_H

#include <stdint.h>

/* Execute instructions until global_cycles reaches deadline.
 * Returns RUN_RES_BREAK if the chain should break (e.g. idle skip),
 * otherwise returns RUN_RES_NORMAL. */
int run_interpreter_chain(uint64_t deadline);

/* Reset interpreter-internal state (branch pipeline).
 * Call before starting a new isolated execution context. */
void interpreter_reset_state(void);

#endif /* INTERPRETER_H */
