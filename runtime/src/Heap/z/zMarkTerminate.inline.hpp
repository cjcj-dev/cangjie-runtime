// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMark.hpp"
#include "Heap/z/zMark.hpp"
#include "Base/Log.h"
#include "Common/SuspendibleThreadSet.h"
namespace MapleRuntime {
void MarkTerminate::Reset(size_t workers)
{
    CHECK_DETAIL(workers != 0, "mark termination needs a worker");
    ZLocker<ZConditionLock> lock(&mutex);
    workerCount = workers;
    working = workers;
    awakening = 0;
}

void MarkTerminate::Leave()
{
    SuspendibleThreadSetLeaver stsLeaver;
    ZLocker<ZConditionLock> lock(&mutex);
    CHECK_DETAIL(working != 0, "mark worker left twice");
    --working;
    if (working == 0) {
        mutex.notify_all();
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
    ZLocker<ZConditionLock> lock(&mutex);
    CHECK_DETAIL(working != 0, "mark worker left termination twice");
    --working;
    if (working == 0) {

        mutex.notify_all();
        return true;
    }
    MaybeReduceStripes(stripes, usedNStripes);
    mutex.wait();
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
    ZLocker<ZConditionLock> lock(&mutex);
    if (working == 0) {
        return;
    }
    if (working + awakening == workerCount) {
        return;
    }
    ++awakening;
    mutex.notify();
}

bool MarkTerminate::Saturated() const
{
    ZLocker<ZConditionLock> lock(&mutex);
    return working + awakening == workerCount;
}

} // namespace MapleRuntime
