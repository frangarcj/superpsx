#ifndef SIO_H
#define SIO_H

#include <stdint.h>

uint32_t SIO_Read(uint32_t phys);
void SIO_Write(uint32_t phys, uint32_t data);

#endif
