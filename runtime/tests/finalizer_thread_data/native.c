#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

typedef uintptr_t (*ThreadDataEntry)(void);
static ThreadDataEntry currentThreadData;

int64_t PrepareThreadDataProbe(void)
{
    currentThreadData = (ThreadDataEntry)dlsym(RTLD_DEFAULT, "MRT_GetThreadLocalData");
    if (currentThreadData == NULL) {
        puts("FINALIZER_THREAD_DATA_INPUT_MISSING MRT_GetThreadLocalData");
        return 1;
    }
    return 0;
}

int64_t RequireCurrentThreadData(void)
{
    uintptr_t managed;
    __asm__ volatile("movq %%r15, %0" : "=r"(managed));
    uintptr_t current = currentThreadData();
    if (managed != current) {
        dprintf(STDOUT_FILENO, "FINALIZER_THREAD_DATA_TARGET stale managed=%#lx current=%#lx\n",
                (unsigned long)managed, (unsigned long)current);
        _exit(3);
    }
    return 0;
}
