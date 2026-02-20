#ifndef SPU_H
#define SPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void SPU_Init(void);
void SPU_Shutdown(void);
uint16_t SPU_ReadReg(uint32_t offset);
void SPU_WriteReg(uint32_t offset, uint16_t value);
void SPU_DMA4(uint32_t madr, uint32_t bcr, uint32_t chcr);
void SPU_GenerateSamples(void);

#ifdef __cplusplus
}
#endif

#endif /* SPU_H */
