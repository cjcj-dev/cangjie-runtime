// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Common/ScopedObjectAccess.h"
#include <cstring>
namespace MapleRuntime {
std::mutex ConcurrentGCBreakpoints::mutex;
std::condition_variable ConcurrentGCBreakpoints::condition;
const char* ConcurrentGCBreakpoints::runTo = nullptr;
bool ConcurrentGCBreakpoints::wantIdle = false;
bool ConcurrentGCBreakpoints::stopped = false;
bool ConcurrentGCBreakpoints::idle = true;
void ConcurrentGCBreakpoints::ResetRequestState()
{
    runTo = nullptr;
    wantIdle = false;
    stopped = false;
}
bool ConcurrentGCBreakpoints::IsControlled()
{
    return wantIdle || stopped || runTo != nullptr;
}
void ConcurrentGCBreakpoints::RunToIdleImpl(bool acquiring)
{
    ScopedEnterSaferegion safe(false);
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(acquiring ? !IsControlled() : IsControlled());
    ResetRequestState();
    wantIdle = true;
    condition.notify_all();
    while (!idle) {
        condition.wait(lock);
    }
}
void ConcurrentGCBreakpoints::AcquireControl() { RunToIdleImpl(true); }
void ConcurrentGCBreakpoints::RunToIdle() { RunToIdleImpl(false); }
void ConcurrentGCBreakpoints::ReleaseControl()
{
    ScopedEnterSaferegion safe(false);
    std::lock_guard<std::mutex> lock(mutex);
    ResetRequestState();
    condition.notify_all();
}
bool ConcurrentGCBreakpoints::RunTo(const char* name)
{
    CHECK(name != nullptr);
    ScopedEnterSaferegion safe(false);
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(IsControlled());
    ResetRequestState();
    runTo = name;
    condition.notify_all();
    if (idle) {
        lock.unlock();
        Heap::GetHeap().RequestGC(GC_REASON_WB_BREAKPOINT);
        lock.lock();
    }
    for (;;) {
        if (wantIdle) {
            return false;
        }
        if (stopped) {
            return true;
        }
        condition.wait(lock);
    }
}
void ConcurrentGCBreakpoints::At(const char* name)
{
    CHECK(name != nullptr);
    std::unique_lock<std::mutex> lock(mutex);
    if (runTo == nullptr || std::strcmp(runTo, name) != 0) {
        return;
    }
    runTo = nullptr;
    stopped = true;
    condition.notify_all();
    while (stopped) {
        condition.wait(lock);
    }
}
void ConcurrentGCBreakpoints::NotifyIdleToActive()
{
    idle = false;
}

void ConcurrentGCBreakpoints::NotifyActiveToIdle()
{
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(!stopped);
    if (runTo != nullptr) {
        runTo = nullptr;
        wantIdle = true;
    }
    idle = true;
    condition.notify_all();
}
}
