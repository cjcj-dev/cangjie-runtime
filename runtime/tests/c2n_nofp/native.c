#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
static _Atomic int started, complete;
static int cleaned;
static void clobber(void) {
    __asm__ volatile (
        "pxor %%xmm0, %%xmm0\n\t"
        "pxor %%xmm1, %%xmm1\n\t"
        "pxor %%xmm2, %%xmm2\n\t"
        "pxor %%xmm3, %%xmm3\n\t"
        "pxor %%xmm4, %%xmm4\n\t"
        "pxor %%xmm5, %%xmm5\n\t"
        "pxor %%xmm6, %%xmm6\n\t"
        "pxor %%xmm7, %%xmm7\n\t"
        "pxor %%xmm8, %%xmm8\n\t"
        "pxor %%xmm9, %%xmm9\n\t"
        "pxor %%xmm10, %%xmm10\n\t"
        "pxor %%xmm11, %%xmm11\n\t"
        "pxor %%xmm12, %%xmm12\n\t"
        "pxor %%xmm13, %%xmm13\n\t"
        "pxor %%xmm14, %%xmm14\n\t"
        "pxor %%xmm15, %%xmm15\n\t"
        : : : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}
int64_t NoFPZero(void) { clobber(); return 29; }
int64_t NoFPOne(int64_t x) { clobber(); return x * 3 + 1; }
int64_t NoFPSix(int64_t a,int64_t b,int64_t c,int64_t d,int64_t e,int64_t f) {
    clobber(); return a+2*b+3*c+4*d+5*e+6*f;
}
void *NoFPPointer(void *p) { clobber(); return p; }
void NoFPVoid(void) { clobber(); }
double FPControl(double x) { return x * 2.0 + 0.5; }
int64_t StackControl(int64_t a,int64_t b,int64_t c,int64_t d,int64_t e,int64_t f,int64_t g) {
    return a+b+c+d+e+f+g;
}
void NoFPReset(void) { atomic_store(&started,0); atomic_store(&complete,0); }
static int wait_flag(_Atomic int *p) {
    const struct timespec delay={0,1000000};
    for(int i=0;i<10000;i++) { if(atomic_load(p))return 1; nanosleep(&delay,0); }
    return 0;
}
int NoFPWaitStarted(void) { return wait_flag(&started); }
void NoFPComplete(void) { atomic_store(&complete,1); }
int NoFPWaitGC(void) { atomic_store(&started,1); return wait_flag(&complete); }
int FPWaitGC(double control) { return control==1.25 ? NoFPWaitGC() : -1; }
void NoFPCallback(void (*callback)(void)) { cleaned=0; callback(); cleaned=37; clobber(); }
int NoFPCleaned(void) { return cleaned; }
