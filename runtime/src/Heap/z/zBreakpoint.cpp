// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/z/zBreakpoint.hpp"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "Mutator/Handshake.h"
#include "Base/Log.h"
#include <chrono>
namespace MapleRuntime {
bool ZBreakpoint::startGC = false;
void ZBreakpoint::StartGC()
{
    std::lock_guard<std::mutex> lock(ConcurrentGCBreakpoints::mutex);
    CHECK(ConcurrentGCBreakpoints::IsControlled());
    CHECK(!startGC);
    startGC = true;
    ConcurrentGCBreakpoints::condition.notify_all();
}
void ZBreakpoint::AtBeforeGC()
{
    std::unique_lock<std::mutex> lock(ConcurrentGCBreakpoints::mutex);
    while (ConcurrentGCBreakpoints::IsControlled() && !startGC) {
        lock.unlock();
        Handshake::Current().process_by_self();
        lock.lock();
        if (!(ConcurrentGCBreakpoints::IsControlled() && !startGC)) {
            break;
        }
        (void)ConcurrentGCBreakpoints::condition.wait_for(lock, std::chrono::milliseconds(1));
    }
    startGC = false;
    ConcurrentGCBreakpoints::NotifyIdleToActive();
}
void ZBreakpoint::AtAfterGC() { ConcurrentGCBreakpoints::NotifyActiveToIdle(); }
void ZBreakpoint::AtAfterMarkingStarted()
{
    ConcurrentGCBreakpoints::At("AFTER MARKING STARTED");
}
void ZBreakpoint::AtBeforeMarkingCompleted()
{
    ConcurrentGCBreakpoints::At("BEFORE MARKING COMPLETED");
}
void ZBreakpoint::AtAfterReferenceProcessingStarted()
{
    ConcurrentGCBreakpoints::At("AFTER CONCURRENT REFERENCE PROCESSING STARTED");
}
}
