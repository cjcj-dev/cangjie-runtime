// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_RELOCATE_HPP
#define MRT_Z_RELOCATE_HPP

#include <atomic>
#include <array>
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
#include "Heap/z/zValue.inline.hpp"

namespace MapleRuntime {
class ScopedStopTheWorld;

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
class ZRelocationSet;
template<typename T> class ZArray;

// ZGC zRelocate.hpp:79-94 / zRelocate.cpp:309-333.
class ZRelocationTargets {
public:
    static constexpr size_t kAges = kPageAgeCount - 1;
    ZRelocationTargets() : targets(std::array<ZPage*, kAges>{}) {}
    ZPage* get(uint32_t partitionId, PageAge age) const
    {
        return targets.get(partitionId)[static_cast<size_t>(age) - 1];
    }
    void set(uint32_t partitionId, PageAge age, ZPage* page)
    {
        targets.get(partitionId)[static_cast<size_t>(age) - 1] = page;
    }
    template<class F> void apply_and_clear_targets(F fn)
    {
        ZPerNUMAIterator<std::array<ZPage*, kAges>> iter(&targets);
        for (std::array<ZPage*, kAges>* entries; iter.next(&entries);) {
            for (auto& page : *entries) { fn(page); page = nullptr; }
        }
    }
private:
    ZPerNUMA<std::array<ZPage*, kAges>> targets;
};

class ZRelocateSmallAllocator {
public:
    explicit ZRelocateSmallAllocator(ZGeneration* generation) : generation(generation) {}
    ZPage* alloc_and_retire_target_page(ZForwarding* forwarding, ZPage* target);
    void share_target_page(ZPage*, uint32_t) {}
    void free_target_page(ZPage* page);
    uintptr_t alloc_object(ZPage* page, size_t size) const;
    void undo_alloc_object(ZPage* page, uintptr_t addr, size_t size) const;
private:
    ZGeneration* generation;
};

class ZRelocateMediumAllocator {
public:
    ZRelocateMediumAllocator(ZGeneration* generation, ZRelocationTargets* targets)
        : generation(generation), sharedTargets(targets) {}
    ~ZRelocateMediumAllocator();
    ZPage* alloc_and_retire_target_page(ZForwarding* forwarding, ZPage* target);
    void share_target_page(ZPage* page, uint32_t partition);
    void free_target_page(ZPage*) {}
    uintptr_t alloc_object(ZPage* page, size_t size) const;
    void undo_alloc_object(ZPage* page, uintptr_t addr, size_t size) const;
private:
    ZGeneration* generation;
    ZRelocationTargets* sharedTargets;
    std::mutex lock;
    std::condition_variable changed;
    bool inPlace{false};
};

class ZRelocate {
public:
    void relocate(ZRelocationSet* relocation_set);
    static void UpdateRemsetForFields(ZForwarding* forwarding, BaseObject* from, BaseObject* to);
    static bool IsFromObject(BaseObject* object);
    static void RemapYoungRoots();
    static void StartRelocationTasks(ZGenerationId generation);

    explicit ZRelocate(ZGeneration* generation) : generation(generation) {}
    BaseObject* forward_object(ZForwarding* forwarding, BaseObject* object);
    BaseObject* relocate_object(ZForwarding* forwarding, BaseObject* object);
    void synchronize();
    void desynchronize();
    ZPerWorker<ZRelocationTargets>* small_targets() { return &smallTargets; }
    ZPerWorker<ZRelocationTargets>* medium_targets() { return &mediumTargets; }
    ZRelocationTargets* shared_medium_targets() { return &sharedMediumTargets; }
    ZRelocateQueue* queue() { return &relocateQueue; }
    bool is_queue_active() const { return relocateQueue.IsActive(); }
    static PageAge compute_to_age(PageAge fromAge);
    static void flip_age_pages(ZWorkers& workers, const ZArray<ZPage*>* pages);
    static void barrier_promoted_pages(ZWorkers& workers, const ZArray<ZPage*>* flipPromoted,
                                       const ZArray<ZPage*>* relocatePromoted);
private:
    BaseObject* relocate_object_inner(ZForwarding* forwarding, BaseObject* obj);
    static void UpdateRemsetOldToOld(ZForwarding* forwarding, BaseObject* from, BaseObject* to);
    ZGeneration* const generation;
    ZRelocateQueue relocateQueue;
    ZPerWorker<ZRelocationTargets> smallTargets;
    ZPerWorker<ZRelocationTargets> mediumTargets;
    ZRelocationTargets sharedMediumTargets;
};

} // namespace MapleRuntime

#endif // MRT_Z_RELOCATE_HPP
