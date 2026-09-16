// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zMarkingSMR.hpp:34-51
#ifndef MRT_ZMARKINGSMR_HPP
#define MRT_ZMARKINGSMR_HPP
#include <atomic>
#include <cstddef>

#include "Heap/z/zArray.hpp"
#include "Heap/z/zValue.hpp"

namespace MapleRuntime {
class MarkStripeStackListNode;

class MarkingSMR {
private:
    struct WorkerState {
        std::atomic<MarkStripeStackListNode*> _hazard_ptr{ nullptr };
        ZArray<MarkStripeStackListNode*>      _scanned_hazards;
        ZArray<MarkStripeStackListNode*>      _freeing;
    };

    ZPerWorker<WorkerState> _worker_states;

    void reclaim(WorkerState* local_state);

public:
    MarkingSMR();
    ~MarkingSMR();
    MarkingSMR(const MarkingSMR&) = delete;
    MarkingSMR& operator=(const MarkingSMR&) = delete;

    void free();
    void free_node(MarkStripeStackListNode* node);
    std::atomic<MarkStripeStackListNode*>* hazard_ptr();

    // Until the mark-end free() (zMark.cpp:993-996, P09) the engine reclaims
    // the current worker's pending nodes when it terminates (FollowWork).
    void reclaim();

    // White-box evidence for the ABA positive/safe control: pending nodes of
    // the current worker. Production pop uses the same hazard slots; this
    // accessor does not alter reclamation.
    size_t pending_count() const;
};
} // namespace MapleRuntime
#endif
