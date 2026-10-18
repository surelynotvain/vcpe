#ifndef VCPE_TEST_PSPKERNEL_H
#define VCPE_TEST_PSPKERNEL_H
#include <stdint.h>
static inline void sceKernelDcacheWritebackRange(const void *p, unsigned int n) { (void)p; (void)n; }
#endif
