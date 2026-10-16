#pragma once
#define PSP_O_RDONLY 1
#define PSP_SEEK_SET 0
static inline int sceIoOpen(const char *p, int f, int m) { (void)p; (void)f; (void)m; return -1; }
static inline int sceIoClose(int fd) { (void)fd; return 0; }
static inline int sceIoRead(int fd, void *b, int n) { (void)fd; (void)b; (void)n; return -1; }
static inline int sceIoLseek32(int fd, int o, int w) { (void)fd; (void)o; (void)w; return 0; }
