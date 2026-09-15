// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMark.hpp"
#include "Heap/z/zMark.hpp"
#include "Base/Log.h"
namespace MapleRuntime {
void MarkTerminate::Reset(size_t workers)
{
    CHECK_DETAIL(workers != 0, "mark termination needs a worker");
    std::lock_guard<std::mutex> lock(mutex);
    workerCount = workers;
    working = workers;
    awakening = 0;
    terminated = false;
}

void MarkTerminate::Leave()
{
    std::lock_guard<std::mutex> lock(mutex);
    CHECK_DETAIL(working != 0, "mark worker left twice");
    --working;
    if (working == 0) {
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
    std::unique_lock<std::mutex> lock(mutex);
    CHECK_DETAIL(working != 0, "mark worker left termination twice");
    --working;
    if (working == 0) {
        MarkingStacks::VerifyEmpty(stripes.Population());
        terminated = true;
        condition.notify_all();
        return true;
    }
    MaybeReduceStripes(stripes, usedNStripes);
    condition.wait(lock);
    if (awakening != 0) {
        --awakening;
    }
    if (working == 0) {
        return true;
    }
    ++working;
    return false;
}

void MarkTerminate::Wake()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (working == 0) {
        return;
    }
    if (working + awakening == workerCount) {
        return;
    }
    ++awakening;
    condition.notify_one();
}

bool MarkTerminate::Saturated() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return working + awakening == workerCount;
}

bool MarkTerminate::Terminated() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return terminated && working == 0;
}

size_t MarkTerminate::WorkerCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return workerCount;
}


} // namespace MapleRuntime
