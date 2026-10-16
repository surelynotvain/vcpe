/* host stubs for compiling VCPE modules in tests (no PSP) */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef unsigned int SceSize; typedef int SceUID; typedef long long SceInt64; typedef unsigned int u32; typedef unsigned char u8;
#define PSP_THREAD_ATTR_USER 0
#define PSP_THREAD_ATTR_VFPU 0
typedef int (*SceKernelThreadEntry)(SceSize, void *);
int sceKernelCreateThread(const char *n, SceKernelThreadEntry e, int prio, int stack, int attr, void *opt);
int sceKernelStartThread(int th, SceSize len, void *argp);
int sceKernelCreateSema(const char *n, int attr, int init, int max, void *opt);
static inline int sceKernelSignalSema(int s, int n) { (void)s; (void)n; return 0; }
static inline int sceKernelWaitSemaCB(int s, int n, void *t) { (void)s; (void)n; (void)t; return 0; }
static inline int sceKernelDelayThread(int us) { (void)us; return 0; }
static inline void sceKernelDcacheWritebackRange(const void *p, int n) { (void)p; (void)n; }
static inline void sceKernelDcacheWritebackAll(void) {}
