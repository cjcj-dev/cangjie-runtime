#include "Base/Panic.h"
#include "Common/SuspendibleThreadSet.h"
#include "Base/Log.h"
#include "Mutator/ThreadLocal.h"
#include "Base/Semaphore.h"
#include <condition_variable>
#include <mutex>

namespace MapleRuntime {
uint32_t SuspendibleThreadSet::nthreads = 0;
uint32_t SuspendibleThreadSet::nthreadsStopped = 0;
std::atomic<bool> SuspendibleThreadSet::suspendAll{false};
double SuspendibleThreadSet::suspendAllStart = 0.0;

namespace {
std::mutex stsLock;
std::condition_variable stsWait;
Semaphore* synchronizeWakeup = nullptr;

void EnsureWakeup()
{
    if (synchronizeWakeup == nullptr) {
        synchronizeWakeup = new Semaphore();
    }
}
} // namespace

bool SuspendibleThreadSet::is_synchronized()
{
    CHECK_DETAIL(nthreadsStopped <= nthreads, "STS stopped overflow");
    return nthreadsStopped == nthreads;
}

// HotSpot thread.hpp:215-230. This is diagnostic identity, not the STS count.
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
bool SuspendibleThreadSet::is_suspendible_thread()
{
    return ThreadLocal::GetThreadLocalData()->isSuspendibleThread;
}

#endif

void SuspendibleThreadSet::join()
{
    MRT_ASSERT(!is_suspendible_thread(), "STS thread already joined");
    std::unique_lock<std::mutex> lock(stsLock);
    while (should_yield()) {
        stsWait.wait(lock);
    }
    ++nthreads;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    ThreadLocal::GetThreadLocalData()->isSuspendibleThread = true;
#endif
}

void SuspendibleThreadSet::leave()
{
    MRT_ASSERT(is_suspendible_thread(), "STS thread not joined");
    std::unique_lock<std::mutex> lock(stsLock);
    CHECK_DETAIL(nthreads > 0, "STS leave without join");
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    ThreadLocal::GetThreadLocalData()->isSuspendibleThread = false;
#endif
    --nthreads;
    if (should_yield() && is_synchronized()) {
        EnsureWakeup();
        synchronizeWakeup->signal();
    }
}

void SuspendibleThreadSet::yield_slow()
{
    MRT_ASSERT(is_suspendible_thread(), "STS yield requires membership");
    std::unique_lock<std::mutex> lock(stsLock);
    if (should_yield()) {
        ++nthreadsStopped;
        if (is_synchronized()) {
            EnsureWakeup();
            synchronizeWakeup->signal();
        }
        while (should_yield()) {
            stsWait.wait(lock);
        }
        CHECK_DETAIL(nthreadsStopped > 0, "STS yield count");
        --nthreadsStopped;
    }
}

void SuspendibleThreadSet::synchronize()
{
    EnsureWakeup();
    {
        std::unique_lock<std::mutex> lock(stsLock);
        CHECK_DETAIL(!should_yield(), "Only one STS synchronize");
        suspendAll.store(true, std::memory_order_relaxed);
        if (is_synchronized()) {
            return;
        }
    }
    synchronizeWakeup->wait();
}

void SuspendibleThreadSet::desynchronize()
{
    std::unique_lock<std::mutex> lock(stsLock);
    CHECK_DETAIL(should_yield(), "STS not synchronizing");
    CHECK_DETAIL(is_synchronized(), "STS not synchronized");
    suspendAll.store(false, std::memory_order_relaxed);
    stsWait.notify_all();
}
void ZRendezvousGCThreads::doit()
{
    SuspendibleThreadSet::synchronize();
    SuspendibleThreadSet::desynchronize();
}

bool ZRendezvousGCThreads::skip_thread_oop_barriers() const
{
    CHECK_DETAIL(false, "Concurrent VMOps should not call skip_thread_oop_barriers");
    return true;
}
} // namespace MapleRuntime
