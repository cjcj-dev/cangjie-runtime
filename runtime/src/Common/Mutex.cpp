// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Common/Mutex.h"
#include "Common/ScopedObjectAccess.h"
#include "Base/Panic.h"
#include "Mutator/ThreadLocal.h"

namespace MapleRuntime {
class InFlightMutexRelease {
public:
    explicit InFlightMutexRelease(Mutex& mutex) : mutex(mutex) {}
    void operator()()
    {
        // Ownership has not yet been published, or was cleared before wait.
        mutex.platformLock.unlock();
        released = true;
    }
    bool not_released() const { return !released; }
private:
    Mutex& mutex;
    bool released = false;
};

bool Mutex::owned_by_self() const
{
    return owner.load(std::memory_order_acquire) == ThreadLocal::GetThreadLocalData();
}

void Mutex::lock_contended()
{
    Mutator* const mutator = Mutator::GetMutator();
    do {
        if (mutator != nullptr && !mutator->InSaferegion()) {
            InFlightMutexRelease release(*this);
            {
                MutatorBlockInVMPreprocess<InFlightMutexRelease> blocked(*mutator, release);
                platformLock.lock();
            }
            if (release.not_released()) { break; }
        } else {
            platformLock.lock();
            break;
        }
    } while (!platformLock.try_lock());
}

void Mutex::lock()
{
    CHECK_DETAIL(!owned_by_self(), "recursive runtime mutex acquisition");
    if (!platformLock.try_lock()) { lock_contended(); }
    CHECK_DETAIL(!is_locked(), "runtime mutex has an owner after acquisition");
    set_owner(ThreadLocal::GetThreadLocalData());
}

void Mutex::unlock()
{
    CHECK_DETAIL(owned_by_self(), "runtime mutex unlock requires ownership");
    set_owner(nullptr);
    platformLock.unlock();
}

void Monitor::wait()
{
    CHECK_DETAIL(owned_by_self(), "runtime monitor wait requires ownership");
    set_owner(nullptr);
    Mutator* const mutator = Mutator::GetMutator();
    if (mutator != nullptr && !mutator->InSaferegion()) {
        InFlightMutexRelease release(*this);
        {
            MutatorBlockInVMPreprocess<InFlightMutexRelease> blocked(*mutator, release);
            std::unique_lock<std::mutex> guard(platformLock, std::adopt_lock);
            condition.wait(guard);
            guard.release();
        }
        if (!release.not_released()) {
            lock();
            return;
        }
    } else {
        std::unique_lock<std::mutex> guard(platformLock, std::adopt_lock);
        condition.wait(guard);
        guard.release();
    }
    CHECK_DETAIL(!is_locked(), "runtime monitor has an owner after wait");
    set_owner(ThreadLocal::GetThreadLocalData());
}

void Monitor::notify_all()
{
    CHECK_DETAIL(owned_by_self(), "runtime monitor notify requires ownership");
    condition.notify_all();
}

Monitor& HeapMonitor()
{
    static Monitor* const monitor = new Monitor();
    return *monitor;
}
} // namespace MapleRuntime
