// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMark.hpp"
#include "Base/Log.h"
#include "Common/SuspendibleThreadSet.h"
namespace MapleRuntime {
void MarkTerminate::Reset(size_t workers)
{
    CHECK_DETAIL(workers != 0, "mark termination needs a worker");
    workerCount = workers;
    working.store(workers, std::memory_order_relaxed);
    awakening.store(0, std::memory_order_relaxed);
}

void MarkTerminate::Leave()
{
    SuspendibleThreadSetLeaver stsLeaver;
    std::lock_guard<std::mutex> lock(mutex);
    if (working.fetch_sub(1, std::memory_order_relaxed) == 1) {
        condition.notify_all();
    }
}

void MarkTerminate::MaybeReduceStripes(MarkStripeSet& stripes, size_t usedNStripes)
{
    const size_t nstripes = stripes.NStripes();
    if (usedNStripes == nstripes && nstripes > 1) {
        (void)stripes.TrySetNStripes(nstripes, nstripes >> 1);
    }
}

bool MarkTerminate::TryTerminate(MarkStripeSet& stripes, size_t usedNStripes)
{
    SuspendibleThreadSetLeaver stsLeaver;
    std::unique_lock<std::mutex> lock(mutex);
    if (working.fetch_sub(1, std::memory_order_relaxed) == 1) {

        condition.notify_all();
        return true;
    }
    MaybeReduceStripes(stripes, usedNStripes);
    condition.wait(lock);
    if (awakening.load(std::memory_order_relaxed) != 0) {
        awakening.fetch_sub(1, std::memory_order_relaxed);
    }
    if (working.load(std::memory_order_relaxed) == 0) {
        return true;
    }
    working.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void MarkTerminate::Wake()
{
    const size_t nworking = working.load(std::memory_order_relaxed);
    const size_t nawakening = awakening.load(std::memory_order_relaxed);
    if (nworking + nawakening == workerCount) {
        return;
    }
    if (nworking == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (working.load(std::memory_order_relaxed) + awakening.load(std::memory_order_relaxed) != workerCount) {
        awakening.fetch_add(1, std::memory_order_relaxed);
        condition.notify_one();
    }
}

bool MarkTerminate::Saturated() const
{
    const size_t nworking = working.load(std::memory_order_relaxed);
    const size_t nawakening = awakening.load(std::memory_order_relaxed);
    return nworking + nawakening == workerCount;
}

} // namespace MapleRuntime
