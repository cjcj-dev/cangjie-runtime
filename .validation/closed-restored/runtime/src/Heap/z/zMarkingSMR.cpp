// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zMarkingSMR.cpp:29-112
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zArray.inline.hpp"
#include "Heap/z/zValue.inline.hpp"
#include "Heap/z/workerThread.hpp"
#include "Base/Log.h"

namespace MapleRuntime {
MarkingSMR::MarkingSMR()
    : _worker_states() {}

MarkingSMR::~MarkingSMR()
{
    free();
}

void MarkingSMR::reclaim(WorkerState* const local_state)
{
    ZArray<MarkStripeStackListNode*>* const freeing = &local_state->_freeing;
    ZPerWorkerIterator<WorkerState> iter(&_worker_states);
    ZArray<MarkStripeStackListNode*>* const scanned_hazards = &local_state->_scanned_hazards;

    for (WorkerState* remote_state; iter.next(&remote_state);) {
        MarkStripeStackListNode* const hazard = remote_state->_hazard_ptr.load(std::memory_order_acquire);

        if (hazard != nullptr) {
            scanned_hazards->append(hazard);
        }
    }

    int kept = 0;
    for (int i = 0; i < freeing->length(); ++i) {
        MarkStripeStackListNode* node = freeing->at(i);
        freeing->at_put(i, nullptr);

        if (scanned_hazards->contains(node)) {
            // Keep
            freeing->at_put(kept++, node);
        } else {
            // Delete
            delete node;
        }
    }

    scanned_hazards->clear();
    freeing->trunc_to(kept);
}

void MarkingSMR::free_node(MarkStripeStackListNode* node)
{
    // We use hazard pointers as an safe memory reclamation (SMR) technique,
    // for marking stacks. Each stripe has a lock-free stack of mark stacks.
    // When a GC thread (1) pops a mark stack from this lock-free stack,
    // there is a small window of time when the head has been read and we
    // are about to read its next pointer. It is then of great importance
    // that the node is not concurrently freed by another concurrent GC
    // thread (2), popping the same entry. Using hazard pointers involves
    // publishing what head was observed by GC thread (1), so that GC thread
    // (2) knows not to free the node when popping it in this race
    // (zMarkingSMR.cpp:35-57).

    CHECK_DETAIL(WorkerThread::worker_id() < ZPerWorkerStorage::count(), "must be a worker");

    WorkerState* const local_state = _worker_states.addr();
    ZArray<MarkStripeStackListNode*>* const freeing = &local_state->_freeing;
    freeing->append(node);

    if (freeing->length() < (int)ZPerWorkerStorage::count() * 8) {
        return;
    }

    reclaim(local_state);
}

void MarkingSMR::reclaim()
{
    CHECK_DETAIL(WorkerThread::worker_id() < ZPerWorkerStorage::count(), "must be a worker");
    reclaim(_worker_states.addr());
}

void MarkingSMR::free()
{
    // Here it is free by definition to free mark stacks.
    ZPerWorkerIterator<WorkerState> iter(&_worker_states);
    for (WorkerState* worker_state; iter.next(&worker_state);) {
        ZArray<MarkStripeStackListNode*>* const freeing = &worker_state->_freeing;
        for (MarkStripeStackListNode* node : *freeing) {
            delete node;
        }
        freeing->clear();
    }
}

std::atomic<MarkStripeStackListNode*>* MarkingSMR::hazard_ptr()
{
    CHECK_DETAIL(WorkerThread::worker_id() < ZPerWorkerStorage::count(), "must be a worker");
    return &_worker_states.addr()->_hazard_ptr;
}

size_t MarkingSMR::pending_count() const
{
    CHECK_DETAIL(WorkerThread::worker_id() < ZPerWorkerStorage::count(), "must be a worker");
    return static_cast<size_t>(_worker_states.addr()->_freeing.length());
}
} // namespace MapleRuntime
