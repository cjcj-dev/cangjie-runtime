// Compile against the product header; the named wrappers also expose its LLVM CAS orders.
#include "Base/RwLock.h"
#include <cstdio>
#include <thread>
#include <vector>
#include <cstdlib>
using MapleRuntime::RwLock;
extern "C" __attribute__((noinline)) void NativeSafetyLockWrite(RwLock* lock) { lock->LockWrite(); }
extern "C" __attribute__((noinline)) bool NativeSafetyTryLockWrite(RwLock* lock) { return lock->TryLockWrite(); }
static void Require(bool value) { if (!value) std::abort(); }
int main()
{
    RwLock lock;
    lock.LockRead();
    Require(!NativeSafetyTryLockWrite(&lock));
    lock.UnlockRead();
    NativeSafetyLockWrite(&lock);
    Require(!NativeSafetyTryLockWrite(&lock));
    Require(!lock.TryLockRead());
    lock.UnlockWrite();
    while (!NativeSafetyTryLockWrite(&lock)) {}
    lock.UnlockWrite();
    unsigned value = 0;
    unsigned complement = ~0u;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 2; ++t) {
        workers.emplace_back([&, t] {
            for (unsigned i = 0; i < 10000; ++i) {
                if (t == 0) NativeSafetyLockWrite(&lock);
                else while (!NativeSafetyTryLockWrite(&lock)) std::this_thread::yield();
                Require(complement == ~value);
                ++value;
                complement = ~value;
                lock.UnlockWrite();
            }
        });
    }
    for (auto& worker : workers) worker.join();
    lock.LockRead();
    Require(value == 20000 && complement == ~value);
    lock.UnlockRead();
    std::puts("PASS RwLock: exclusion, writer handoff, final reader; x86 execution is not a weak-memory proof");
}
