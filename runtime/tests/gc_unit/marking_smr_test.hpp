// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Heap/z/zMarkingSMR.hpp"
#if defined(MRT_TESTABLE_INTERNALS)
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zArray.inline.hpp"
#include "Heap/z/zValue.inline.hpp"
#include <vector>
#endif
namespace MapleRuntime {
class MarkingSMRTest {
public:
#if defined(MRT_TESTABLE_INTERNALS)
    // Fixture preparation only: two active workers retire nodes protected by
    // two inactive worker slots. Collection must consume the resulting state.
    static void prepare_protected_nodes(MarkingSMR& smr)
    {
        for (uint32_t worker = 0; worker < 2; ++worker) {
            auto* node = new MarkStripeStackListNode(nullptr);
            smr._worker_states.addr(worker)->_freeing.append(node);
            smr._worker_states.addr(worker + 2)->_hazard_ptr.store(node, std::memory_order_release);
        }
    }
    static std::vector<size_t> worker_pending_counts(MarkingSMR& smr)
    {
        std::vector<size_t> counts;
        ZPerWorkerIterator<MarkingSMR::WorkerState> iter(&smr._worker_states);
        for (MarkingSMR::WorkerState* state; iter.next(&state);) {
            counts.push_back(static_cast<size_t>(state->_freeing.length()));
        }
        return counts;
    }
    static void clear_fixture_hazards(MarkingSMR& smr)
    {
        for (uint32_t worker = 2; worker < 4; ++worker) {
            smr._worker_states.addr(worker)->_hazard_ptr.store(nullptr, std::memory_order_release);
        }
    }
#endif
    static void reclaim(MarkingSMR& smr) { smr.reclaim(); }
    static size_t pending_count(const MarkingSMR& smr) { return smr.pending_count(); }
};
}
