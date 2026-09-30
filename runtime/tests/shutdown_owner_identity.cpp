#include "Cangjie.h"
#include <cstdio>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" bool MRT_NewForeignCJThread();
unsigned long shutdownOwnerPthread = 0;
unsigned long shutdownReusedPthread = 0;
int shutdownOwnerResult = -1;
int shutdownOwnerObservationDone = 0;

extern "C" __attribute__((noinline)) void ShutdownOwnerReuseReady()
{
    std::fprintf(stderr, "SHUTDOWN_OWNER_REUSE_PRECONDITION owner=%lu reused=%lu equal=%d fini=%d\n",
                 shutdownOwnerPthread, shutdownReusedPthread,
                 shutdownOwnerPthread == shutdownReusedPthread, shutdownOwnerResult);
}

static void* FinishRuntime(void*)
{
    shutdownOwnerPthread = static_cast<unsigned long>(pthread_self());
    shutdownOwnerResult = FiniCJRuntime();
    return nullptr;
}

static void* ReenterRuntime(void*)
{
    shutdownReusedPthread = static_cast<unsigned long>(pthread_self());
    ShutdownOwnerReuseReady();
    MRT_NewForeignCJThread();
    std::fprintf(stderr, "SHUTDOWN_OWNER_REUSE_UNEXPECTED_RETURN\n");
    return nullptr;
}

int main()
{
    RuntimeParam parameters{};
    parameters.heapParam.heapSize = 64 * 1024;
    parameters.coParam.processorNum = 1;
    if (InitCJRuntime(&parameters) != E_OK) { return 2; }
    constexpr size_t stackSize = 8 * 1024 * 1024;
    void* stack = mmap(nullptr, stackSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) { return 3; }
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0 ||
        pthread_attr_setstack(&attributes, stack, stackSize) != 0) { return 4; }
    pthread_t owner;
    if (pthread_create(&owner, &attributes, FinishRuntime, nullptr) != 0 ||
        pthread_join(owner, nullptr) != 0 || shutdownOwnerResult != E_OK) { return 5; }
    pthread_t reentry;
    if (pthread_create(&reentry, &attributes, ReenterRuntime, nullptr) != 0) { return 6; }
    pthread_attr_destroy(&attributes);
    pthread_detach(reentry);
    while (__atomic_load_n(&shutdownOwnerObservationDone, __ATOMIC_ACQUIRE) == 0) {
        usleep(1000);
    }
    _exit(0);
}
