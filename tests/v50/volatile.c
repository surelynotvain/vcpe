#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include "vcpe_mem.h"
static int locked, fail_unlock, locks, unlocks;
static uint8_t memory[64];
volatile int vcpe_resume_gen;
int vcpe_step_begin(const char *fmt, ...) { (void)fmt; return 0; }
void vcpe_step_end(int id, int ok) { (void)id; (void)ok; }
int sceKernelVolatileMemTryLock(int kind, void **p, int *n)
{
    (void)kind; assert(!locked); locked=1; locks++; *p=memory; *n=sizeof memory; return 0;
}
int sceKernelVolatileMemUnlock(int kind)
{
    (void)kind; assert(locked); if(fail_unlock) return -1; locked=0; unlocks++; return 0;
}
int main(void)
{
    uint32_t n=0;
    assert(vcpe_volatile_suspend()==0 && locks==0);
    assert(vcpe_volatile_mem(&n)==memory && n==64 && locks==1);
    fail_unlock=1; assert(vcpe_volatile_suspend()==-1 && locked);
    fail_unlock=0; assert(vcpe_volatile_suspend()==1 && !locked && unlocks==1);
    assert(vcpe_volatile_mem(&n)==0 && n==0 && locks==1 && vcpe_volatile_suspended());
    assert(vcpe_volatile_suspend()==-1);
    assert(vcpe_volatile_resume()==0 && locked && locks==2 && vcpe_resume_gen==1 && !vcpe_volatile_suspended());
    assert(vcpe_volatile_resume()==0 && locks==2 && vcpe_resume_gen==1);
    assert(vcpe_volatile_mem(&n)==memory && n==64);
    puts("PASS volatile handoff: ownership, unlock failure, paused access, reclaim and generation invalidation");
}
