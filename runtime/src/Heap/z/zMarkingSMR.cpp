// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMarkStack.hpp"
#include "Base/Log.h"
#include <algorithm>
namespace MapleRuntime {
MarkingSMR::MarkingSMR(size_t workerCount)
    : workerCount(workerCount), workers(new (std::nothrow) WorkerState[workerCount])
{
    CHECK_DETAIL(workerCount != 0, "marking SMR needs at least one worker");
    CHECK_DETAIL(workers != nullptr, "failed to allocate marking SMR states workers=%zu", workerCount);
}

MarkingSMR::~MarkingSMR()
{
    Free();
}

std::atomic<MarkStripeStackListNode*>& MarkingSMR::Hazard(size_t workerId)
{
    CHECK_DETAIL(workerId < workerCount, "invalid SMR worker id=%zu count=%zu", workerId, workerCount);
    return workers[workerId].hazard;
}

void MarkingSMR::Retire(size_t workerId, MarkStripeStackListNode* node)
{
    CHECK_DETAIL(workerId < workerCount, "invalid SMR retire worker id=%zu count=%zu", workerId, workerCount);
    WorkerState& local = workers[workerId];
    local.freeing.push_back(node);
    if (local.freeing.size() >= workerCount * 8) {
        Reclaim(workerId);
    }
}

void MarkingSMR::Reclaim(size_t workerId)
{
    CHECK_DETAIL(workerId < workerCount, "invalid SMR reclaim worker id=%zu count=%zu", workerId, workerCount);
    WorkerState& local = workers[workerId];
    for (size_t i = 0; i < workerCount; ++i) {
        MarkStripeStackListNode* const hazard = workers[i].hazard.load(std::memory_order_acquire);
        if (hazard != nullptr) {
            local.scannedHazards.push_back(hazard);
        }
    }

    size_t kept = 0;
    for (MarkStripeStackListNode* node : local.freeing) {
        if (std::find(local.scannedHazards.begin(), local.scannedHazards.end(), node) !=
            local.scannedHazards.end()) {
            local.freeing[kept++] = node;
        } else {
            delete node;
        }
    }
    local.scannedHazards.clear();
    local.freeing.resize(kept);
}

void MarkingSMR::Free()
{
    if (workers == nullptr) {
        return;
    }
    // Called only after all mark workers have joined. At that point no hazard
    // can be legitimately held and all delayed nodes can be freed directly.
    for (size_t i = 0; i < workerCount; ++i) {
        CHECK_DETAIL(workers[i].hazard.load(std::memory_order_relaxed) == nullptr,
                     "marking SMR worker %zu still holds a hazard during Free", i);
        for (MarkStripeStackListNode* node : workers[i].freeing) {
            delete node;
        }
        workers[i].freeing.clear();
        workers[i].scannedHazards.clear();
    }
}

size_t MarkingSMR::PendingCount(size_t workerId) const
{
    CHECK_DETAIL(workerId < workerCount, "invalid SMR pending worker id=%zu count=%zu", workerId, workerCount);
    return workers[workerId].freeing.size();
}


} // namespace MapleRuntime
