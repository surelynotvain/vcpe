#include <stdint.h>
long long sceKernelGetSystemTimeWide(void);
uint32_t sceKernelGetSystemTimeLow(void);
void sceKernelDcacheWritebackRange(const void *,unsigned int);
void sceKernelDcacheWritebackAll(void);
