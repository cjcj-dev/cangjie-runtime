// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_RELOCATE_HPP
#define MRT_Z_RELOCATE_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

#include "Common/TypeDef.h"
#include "Heap/z/zArray.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zPageAge.hpp"

namespace MapleRuntime {

// ZRelocateQueue (zRelocate.hpp:39-77; zRelocate.cpp:57-307).
class ZRelocateQueue {
public:
    enum class State : uint8_t { QUEUED, CLAIMED, COMPLETED };

    struct EnqueueResult {
        ZForwarding* forwarding;
        bool inserted;
        bool accepted;
        ZForwarding* request;
        State state() const
        {
            if (forwarding == nullptr) {
                return State::QUEUED;
            }
            if (forwarding->is_done()) {
                return State::COMPLETED;
            }
            return forwarding->claimed().load(std::memory_order_acquire) ? State::CLAIMED : State::QUEUED;
        }
    };

    struct Selection {
        ZForwarding* forwarding;
        void* ordinary;
        bool workersDone{ false };

        bool is_request() const { return forwarding != nullptr; }
        void* owner() const { return forwarding != nullptr ? forwarding->page() : nullptr; }
        explicit operator bool() const { return forwarding != nullptr || ordinary != nullptr; }
    };

    void activate(uint32_t nworkers);
    void deactivate();
    bool is_active() const;
    void join(uint32_t nworkers);
    void resize_workers(uint32_t nworkers);
    void leave();
    void add_and_wait(ZForwarding* forwarding);
    ZForwarding* synchronize_poll();
    void synchronize();
    void desynchronize();
    void clear();

    void BeginWorkers(size_t workers) { activate(static_cast<uint32_t>(workers)); }
    EnqueueResult Add(void* owner, MAddress from);
    EnqueueResult Add(ZForwarding* forwarding);
    void Wait(ZForwarding* forwarding);
    size_t Complete(ZForwarding* forwarding);
    ZForwarding* PruneAndClaim();
    Selection SelectBeforeOrdinary(const std::function<void*()>& claimOrdinary)
    {
        ZForwarding* forwarding = PruneAndClaim();
        if (forwarding != nullptr) {
            return Selection{ forwarding, nullptr, false };
        }
        return Selection{ nullptr, claimOrdinary(), false };
    }
    Selection SynchronizePoll();
    bool IsActive() const { return is_active(); }
    size_t PendingCount() const;
    size_t SynchronizedWorkerCount() const;
    uint64_t CompletionCount() const { return completionCount.load(std::memory_order_relaxed); }

#if defined(MRT_TESTABLE_INTERNALS)
    using WaitEnterHook = void (*)(ZForwarding* forwarding);
    static void SetWaitEnterHook(WaitEnterHook hook);
#endif

private:
    bool needs_attention() const;
    void inc_needs_attention();
    void dec_needs_attention();
    bool prune();
    ZForwarding* prune_and_claim();
    void synchronize_thread();
    void desynchronize_thread();

    mutable std::mutex lock;
    std::condition_variable attention;
    ZArray<ZForwarding*> queue;
    uint32_t nworkers{ 0 };
    uint32_t nsynchronized{ 0 };
    bool synchronizeFlag{ false };
    std::atomic<bool> isActive{ false };
    std::atomic<int> needsAttention{ 0 };
    std::atomic<uint64_t> completionCount{ 0 };
};

class ZWorkers;
class ZPage;
class ZGeneration;
struct ForwardingProvenance;
template<typename T> class ZArray;

class ZRelocationTargets {
public:
    static constexpr size_t kAges = 16;
    ZPage* get(uint32_t partitionId, PageAge age) const
    {
        (void)partitionId;
        return targets[static_cast<size_t>(age) % kAges];
    }
    void set(uint32_t partitionId, PageAge age, ZPage* page)
    {
        (void)partitionId;
        targets[static_cast<size_t>(age) % kAges] = page;
    }
private:
    ZPage* targets[kAges]{};
};

class ZRelocate {
public:
    explicit ZRelocate(ZGeneration* generation) : generation(generation) {}
    BaseObject* relocate_object(ZForwarding* forwarding, BaseObject* object,
                                const ForwardingProvenance& provenance);
    ZRelocateQueue* queue() { return &relocateQueue; }
    bool is_queue_active() const { return relocateQueue.IsActive(); }
    static PageAge compute_to_age(PageAge fromAge);
    static void flip_age_pages(ZWorkers& workers, const ZArray<ZPage*>* pages);
    static void barrier_promoted_pages(ZWorkers& workers, const ZArray<ZPage*>* flipPromoted,
                                       const ZArray<ZPage*>* relocatePromoted);
private:
    friend class HeapGcState;
#if defined(MRT_TESTABLE_INTERNALS)
    friend struct RelocationReceiptTestAccess;
    friend struct MutatorPublishTestAccess;
#endif
    BaseObject* relocate_object_inner(BaseObject* obj, ZPage* copyPage);
    static void UpdateRemsetForFields(BaseObject* from, BaseObject* to);
    BaseObject* TryMutatorRelocate(BaseObject* obj, ZPage::RetainScope& lease);
    BaseObject* WaitForPageForwarding(BaseObject* obj, ZForwarding* owner) const;
    ZGeneration* const generation;
    ZRelocateQueue relocateQueue;
};

namespace CopyCollectorInternal {
bool ScrubMinorFreeTarget(RefField<>& field, BaseObject* target, bool fromFix);
bool HolderObjectIsLive(BaseObject* holder);
bool SlotHeldByLiveObject(const void* slot);
template <typename SetT, typename KeyT>
bool LedgerInsert(SetT& set, const KeyT& key)
{
    return set.insert(key).second;
}
template <typename SetT, typename KeyT>
size_t LedgerCount(const SetT& set, const KeyT& key)
{
    return set.count(key);
}
}

using namespace CopyCollectorInternal;

} // namespace MapleRuntime

#endif // MRT_Z_RELOCATE_HPP
