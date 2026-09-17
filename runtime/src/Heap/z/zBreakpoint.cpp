// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/z/zBreakpoint.hpp"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "Base/Log.h"
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
        ConcurrentGCBreakpoints::condition.wait(lock);
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
