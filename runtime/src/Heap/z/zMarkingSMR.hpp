// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ZMARKINGSMR_HPP
#define MRT_ZMARKINGSMR_HPP
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace MapleRuntime {
class MarkStripeStackListNode;
class MarkingSMR {
public:
    explicit MarkingSMR(size_t workerCount);
    ~MarkingSMR();
    MarkingSMR(const MarkingSMR&) = delete;
    MarkingSMR& operator=(const MarkingSMR&) = delete;

    size_t WorkerCount() const { return workerCount; }
    std::atomic<MarkStripeStackListNode*>& Hazard(size_t workerId);
    void Retire(size_t workerId, MarkStripeStackListNode* node);
    void Reclaim(size_t workerId);
    void Free();

    // White-box evidence for the ABA positive/safe control. Production pop
    // uses the same hazard slots; these accessors do not alter reclamation.
    size_t PendingCount(size_t workerId) const;

private:
    struct alignas(64) WorkerState {
        std::atomic<MarkStripeStackListNode*> hazard{ nullptr };
        std::vector<MarkStripeStackListNode*> scannedHazards;
        std::vector<MarkStripeStackListNode*> freeing;
    };

    size_t workerCount;
    std::unique_ptr<WorkerState[]> workers;
};
} // namespace MapleRuntime
#endif
