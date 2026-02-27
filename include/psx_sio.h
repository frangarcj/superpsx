#ifndef SIO_H
#define SIO_H

#include <stdint.h>

uint32_t SIO_Read(uint32_t phys);
void SIO_Write(uint32_t phys, uint32_t data);

/* Exposed for JIT inline fast paths */
extern uint32_t sio_data;
extern uint32_t sio_stat;
extern int sio_tx_pending;
extern int sio_state;
extern int sio_response_len;
extern int sio_selected;
extern int sio_irq_pending;
extern volatile uint64_t sio_irq_delay_cycle;

#endif
