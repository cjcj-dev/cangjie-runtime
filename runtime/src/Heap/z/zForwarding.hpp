// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_FORWARDING_LIFE_H
#define MRT_Z_FORWARDING_LIFE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "Base/Log.h"

namespace MapleRuntime {
class ZForwarding;

// ZForwarding's four-piece lifetime (zForwarding.hpp:66-69, zForwarding.cpp:34-194).
//
//   Atomic<bool>    _claimed     claim()
//   ZConditionLock  _ref_lock    wait / notify on the three-state count
//   Atomic<int32_t> _ref_count   retain_page / release_page / claim invert / detach wait
//   Atomic<bool>    _done        mark_done / is_done
//
// The three-state count is the ABA answer: a late reader is refused, it is never
// handed a reused table. 0 is terminal. <0 is claimed (in-place relocate). >0 is live.
//
// The canonical words belong to ZForwarding, owned by the relocation set.
class ZForwardingLife {
public:
    ZForwardingLife() = delete;

    // P2 adapter for the legacy page helpers: they borrow the task's single
    // construction token and leave mark_done to the task's final operation.
    class PageWorkScope {
    public:
        explicit PageWorkScope(ZForwarding* forwarding, bool complete = false);
        ~PageWorkScope();
        PageWorkScope(const PageWorkScope&) = delete;
        PageWorkScope& operator=(const PageWorkScope&) = delete;
    private:
        ZForwarding* previous;
        ZForwarding* forwarding;
        bool complete;
    };
    static ZForwarding* CurrentPageWork();

    enum class Retire : uint32_t {
        DISPEL_GHOST = 0,
        TAKE_GARBAGE = 1,
        RECLAIM_DIRTY = 2,
        RECLAIM_MARK_QUARANTINE = 3,
        RELEASE_REGION = 4,
    };

    // zForwarding.inline.hpp:67-70 -- constructed with claimed=false, ref=1, done=false.
    // The construction 1 is the relocating worker's token; it is dropped at retire.
    static void ResetForForwarding(std::atomic<int32_t>& refCount, std::atomic<bool>& claimed,
                                   std::atomic<bool>& done)
    {
        claimed.store(false, std::memory_order_relaxed);
        done.store(false, std::memory_order_relaxed);
        refCount.store(1, std::memory_order_release);
    }

    // Idle state is not evidence that a forwarding page task completed.
    static void ResetIdle(std::atomic<int32_t>& refCount, std::atomic<bool>& claimed, std::atomic<bool>& done)
    {
        claimed.store(false, std::memory_order_relaxed);
        refCount.store(0, std::memory_order_release);
        done.store(false, std::memory_order_release);
        NotifyAll();
    }

    // zForwarding.cpp:51-53
    static bool claim(std::atomic<bool>& claimed);

    // zForwarding.cpp:188-194
    static void mark_done(std::atomic<bool>& done);

    static bool is_done(const std::atomic<bool>& done);

    // zForwarding.cpp:86-108: the claimed arm must finish add_and_wait
    // before it reports that the source page cannot be retained.
    template<typename Wait>
    static bool retain_page(std::atomic<int32_t>& refCount, Wait wait)
    {
        for (;;) {
            int32_t n = refCount.load(std::memory_order_acquire);
            if (n == 0) {
                return false;
            }
            if (n < 0) {
                wait();
                return false;
            }
            if (refCount.compare_exchange_weak(n, n + 1, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                return true;
            }
        }
    }

    // zForwarding.cpp:134-169
    static void release_page(std::atomic<int32_t>& refCount);

    // zForwarding.cpp:110-131 -- invert n → -n, then wait until -1.
    static void in_place_relocation_claim_page(std::atomic<int32_t>& refCount);

    // zForwarding.cpp:171-181 -- block until the count is 0, then the page may be freed.
    static void detach_page(std::atomic<int32_t>& refCount);

    static void WaitPageDone(ZForwarding* forwarding);

private:
    struct Monitor {
        std::mutex mu;
        std::condition_variable cv;
    };

    static Monitor& Lock()
    {
        static Monitor m;
        return m;
    }

    // Hold the mutex across notify so a waiter that has observed the old count
    // but not yet entered wait cannot miss the signal. Same as ZGC's
    // ZLocker<ZConditionLock> around notify_all (zForwarding.cpp:149,163).
    static void NotifyAll()
    {
        std::lock_guard<std::mutex> guard(Lock().mu);
        Lock().cv.notify_all();
    }

    static void WaitUntilRef(std::atomic<int32_t>& refCount, int32_t expect);

};

} // namespace MapleRuntime

#endif // MRT_Z_FORWARDING_LIFE_H

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_FORWARDING_H
#define MRT_Z_FORWARDING_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Heap/z/zForwardingEntry.hpp"
#include "Heap/z/zForwardingAllocator.hpp"
#include "Heap/z/zAttachedArray.hpp"

#include "Heap/z/zHeap.hpp"

namespace MapleRuntime {

using RegionLifeId = uint64_t;

class RegionInfo;
class LiveInfo;

// zForwarding.hpp:44-110 — one off-heap object per relocated page.
// _entries is a ZAttachedArray sitting after this object (zAttachedArray.inline.hpp:44-54).
// Source-page retention: _ref_count / _ref_lock / _done (zForwarding.cpp:34-194).
// Forwarding storage belongs to the generation relocation set.
class ZForwarding {
public:
    using AttachedArray = ZAttachedArray<ZForwarding, std::atomic<uint64_t>>;
    static constexpr uint32_t kAlignShift = 3;
    static constexpr size_t kNotStored = SIZE_MAX;

    struct Receipt {
        enum class Status : uint8_t {
            INSTALLED,
            EXISTING,
        };

        MAddress address;
        bool installed;
        Status status;
    };

    // zForwarding.cpp:55-84 — the old top and livemap belong to the
    // forwarding/from-page incarnation, not to the reusable page metadata.
    // The carrier is installed before relocation and retired as a unit after
    // the last from-page reader drains.
    struct FromPageView {
        LiveInfo* liveInfo = nullptr;
        uint64_t epoch = 0;
        MAddress topAtStart = 0;
        MAddress markStartAllocPtr = 0;
        uint8_t owner = 1;
        uint8_t largeMarked = 0;
        RegionLifeId lifeId = 0;
    };

    static size_t nentries(size_t objectCountUpperBound);

    static ZForwarding* alloc(size_t liveObjects, MAddress start, MAddress heapBase, size_t regionSize,
                              RegionInfo* page, RegionLifeId pageLifeId = 0,
                              ForwardingAllocator* arena = nullptr);

    // Standalone storage for focused forwarding tests. Product objects use the set arena.
    static ZForwarding* Create(size_t liveObjects, MAddress start, MAddress heapBase, size_t regionSize = 0)
    {
        return alloc(liveObjects, start, heapBase, regionSize, nullptr);
    }

    void Destroy()
    {
        this->~ZForwarding();
        AttachedArray::free(this);
    }

    MAddress start() const;
    size_t size() const;
    size_t regionSize() const { return _size; }
    RegionInfo* page() const;
    RegionLifeId page_life_id() const { return _page_life_id; }
    uint8_t table_generation() const { return _table_generation; }
    void set_table_generation(uint8_t generation) { _table_generation = generation; }
    bool page_life_current() const;
    void verify() const;
    size_t length() const { return _entries.length(); }

    void publish_from_page_view(LiveInfo* liveInfo, uint64_t epoch, MAddress topAtStart,
                                MAddress markStartAllocPtr,
                                uint8_t owner, uint8_t largeMarked, RegionLifeId lifeId)
    {
        // lifeId is the publication word. Readers either reject the zero word
        // or acquire the complete immutable replacement.
        __atomic_store_n(&_from_page.lifeId, static_cast<RegionLifeId>(0), __ATOMIC_RELEASE);
        _from_page.liveInfo = liveInfo;
        _from_page.epoch = epoch;
        _from_page.topAtStart = topAtStart;
        _from_page.markStartAllocPtr = markStartAllocPtr;
        _from_page.owner = owner;
        _from_page.largeMarked = largeMarked;
        __atomic_store_n(&_from_page.lifeId, lifeId, __ATOMIC_RELEASE);
    }

    const FromPageView* from_page_view(RegionLifeId currentLife) const
    {
        const RegionLifeId life = __atomic_load_n(&_from_page.lifeId, __ATOMIC_ACQUIRE);
        return life != 0 && life == currentLife ? &_from_page : nullptr;
    }

    const FromPageView* from_page_snapshot() const
    {
        return __atomic_load_n(&_from_page.lifeId, __ATOMIC_ACQUIRE) == 0 ? nullptr : &_from_page;
    }

    // zPage.inline.hpp:176-185 seqnum bounds livemap/forwarding to one page life.
    // Record the to-region start+regionLifeSeq at insert; consume rejects when
    // InitRegionInfo has bumped that seq (RegionInfo.h:InitRegionInfo).
    static bool DestUsable(MAddress to);


    bool covers(MAddress addr) const { return _size != 0 && addr >= _start && addr < _start + _size; }

    uintptr_t index(MAddress from) const;

    std::atomic<uint64_t>* entries() const;

    ForwardingEntry at(ForwardingCursor* cursor) const;

    ForwardingEntry first(uintptr_t fromIndex, ForwardingCursor* cursor) const;

    ForwardingEntry next(ForwardingCursor* cursor) const;

    // zForwarding.inline.hpp:230-245 plus a bound: our nentries estimate can undersize
    // (REPORT-fwdentries). A miss is a state every caller already handles.
    ForwardingEntry find(uintptr_t fromIndex, ForwardingCursor* cursor) const;

    // In-place relocation reuses one page for both layouts.  ClassifyCompactedMiss
    // must therefore be able to establish that an address below the new top is a
    // published destination before consulting the old liveness face
    // (ZGC zForwarding.cpp:55-64; zHeap.cpp:202-208).
    bool find_from_by_to(MAddress to, MAddress* fromOut) const
    {
        auto* words = entries();
        for (size_t i = 0; i < _entries.length(); ++i) {
            const ForwardingEntry entry = ForwardingEntry::FromRaw(words[i].load(std::memory_order_acquire));
            if (!entry.populated()) {
                continue;
            }
            if (_heapBase + static_cast<MAddress>(entry.to_offset()) == to) {
                if (fromOut != nullptr) {
                    *fromOut = _start + (static_cast<MAddress>(entry.from_index()) << kAlignShift);
                }
                return true;
            }
        }
        std::lock_guard<std::mutex> lock(_overflowLock);
        for (const auto& kv : _overflow) {
            if (kv.second == to) {
                if (fromOut != nullptr) {
                    *fromOut = kv.first;
                }
                return true;
            }
        }
        return false;
    }

    // zForwarding.inline.hpp:248-252 — miss is null, never geometry.
    MAddress find(MAddress from) const;

    template<typename Fn>
    void for_each_from(Fn&& fn) const
    {
        auto* words = entries();
        for (size_t i = 0; i < _entries.length(); ++i) {
            const ForwardingEntry entry = ForwardingEntry::FromRaw(words[i].load(std::memory_order_acquire));
            if (entry.populated()) {
                fn(_start + (static_cast<MAddress>(entry.from_index()) << kAlignShift));
            }
        }
        std::lock_guard<std::mutex> lock(_overflowLock);
        for (const auto& kv : _overflow) {
            fn(kv.first);
        }
    }

    size_t insert(uintptr_t fromIndex, size_t toOffset, ForwardingCursor* cursor, bool* installed = nullptr);

    static std::atomic<uint64_t>& FullFallbacks()
    {
        static std::atomic<uint64_t> n{ 0 };
        return n;
    }

    // Preserve the pre-existing diagnostic contract. These count refusal by
    // the bounded attached array; fallback counters below count the successful
    // exact-key continuation of that same event.
    static std::atomic<uint64_t>& FullRefusals()
    {
        static std::atomic<uint64_t> n{ 0 };
        return n;
    }

    static std::atomic<uint64_t>& OverflowFallbacks()
    {
        static std::atomic<uint64_t> n{ 0 };
        return n;
    }

    static std::atomic<uint64_t>& OverflowRefusals()
    {
        static std::atomic<uint64_t> n{ 0 };
        return n;
    }

    Receipt insert_receipt(MAddress from, MAddress to, const std::function<void()>& beforeFirstCas = {})
    {
        ForwardingCursor cursor = 0;
        const uintptr_t fromIndex = index(from);
        const size_t toOffset = static_cast<size_t>(to - _heapBase);
        const bool encodable = fromIndex <= ForwardingEntry::kMaxFromIndex &&
            toOffset <= ForwardingEntry::kMaxToOffset;
        if (encodable) {
            const ForwardingEntry existing = find(fromIndex, &cursor);
            if (existing.populated()) {
                return Receipt{ _heapBase + static_cast<MAddress>(existing.to_offset()), false,
                                Receipt::Status::EXISTING };
            }
            if (beforeFirstCas) {
                beforeFirstCas();
            }
            bool installed = false;
            const size_t finalOff = insert(fromIndex, toOffset, &cursor, &installed);
            if (finalOff != kNotStored) {
                return Receipt{ _heapBase + static_cast<MAddress>(finalOff), installed,
                                installed ? Receipt::Status::INSTALLED : Receipt::Status::EXISTING };
            }
        } else {
            OverflowRefusals().fetch_add(1, std::memory_order_relaxed);
            OverflowFallbacks().fetch_add(1, std::memory_order_relaxed);
        }

        // The attached array is deliberately bounded, but receipt installation is
        // total. Rare estimate/encoding overflow lives in this per-forwarding map;
        // readers consult it after the lock-free table. Recheck the primary table
        // under the overflow lock so a concurrent CAS winner cannot be shadowed.
        std::lock_guard<std::mutex> lock(_overflowLock);
        if (encodable) {
            ForwardingCursor retryCursor = 0;
            const ForwardingEntry existing = find(fromIndex, &retryCursor);
            if (existing.populated()) {
                return Receipt{ _heapBase + static_cast<MAddress>(existing.to_offset()), false,
                                Receipt::Status::EXISTING };
            }
        }
        auto inserted = _overflow.emplace(from, to);
        return Receipt{ inserted.first->second, inserted.second,
                        inserted.second ? Receipt::Status::INSTALLED : Receipt::Status::EXISTING };
    }

    MAddress insert(MAddress from, MAddress to);

    enum class ZPublishState : int8_t {
        none,
        published,
        reject,
        accept,
    };

    static uint32_t young_seqnum()
    {
        return static_cast<uint32_t>(
            Heap::GetHeap().GetCollector().GetCycleSnapshot(GCCycleGeneration::YOUNG).sequence);
    }

    static bool young_marking()
    {
        const auto young = Heap::GetHeap().GetCollector().GetCycleSnapshot(GCCycleGeneration::YOUNG);
        return young.phase == GCPhase::GC_PHASE_TRACE;
    }

    void relocated_remembered_fields_register(MAddress field);

    bool relocated_remembered_fields_is_concurrently_scanned() const;

    // zForwarding.cpp:relocated_remembered_fields_published_contains.
    // Verification is observational; unlike apply_to_published it never clears.
    bool relocated_remembered_fields_published_contains(MAddress field);

    void relocated_remembered_fields_after_relocate();

    void relocated_remembered_fields_publish();

    void relocated_remembered_fields_notify_concurrent_scan_of();

    template<typename Function>
    void relocated_remembered_fields_apply_to_published(Function function);

    // zForwarding.cpp:51-53 / :86-194. Source-page ownership only.
    bool claim();
    bool is_claimed() const;
    bool in_place() const;
    void set_in_place();
    bool retain_page();
    void release_page();
    void detach_page();
    void mark_done();
    bool is_done() const;
    void in_place_relocation_claim_page();

    std::atomic<int32_t>& ref_count() { return _ref_count; }
    std::atomic<bool>& claimed() { return _claimed; }
    std::atomic<bool>& done() { return _done; }
    std::mutex& ref_lock() const { return _ref_lock; }

private:
    // zForwarding.inline.hpp:59-76
    ZForwarding(RegionInfo* page, MAddress start, MAddress heapBase, size_t regionSize, size_t nentries,
                RegionLifeId pageLifeId);

    const MAddress _start;
    const size_t _size;
    const MAddress _heapBase;
    const AttachedArray _entries;
    RegionInfo* const _page;
    const RegionLifeId _page_life_id;
    // Monotonic per-region-span generation. Written before the table pointer is
    // published, then immutable for the table's lifetime.
    uint8_t _table_generation;
    std::atomic<bool> _claimed;
    std::atomic<bool> _in_place;
    mutable std::mutex _ref_lock;
    std::condition_variable _ref_changed;
    std::atomic<int32_t> _ref_count;
    std::atomic<bool> _done;
    mutable std::mutex _overflowLock;
    std::unordered_map<MAddress, MAddress> _overflow;
    mutable std::mutex _receiptInstallLock;
    FromPageView _from_page;
    std::atomic<ZPublishState> _relocated_remembered_fields_state;
    std::vector<MAddress> _relocated_remembered_fields_array;
    uint32_t _relocated_remembered_fields_publish_young_seqnum;
    mutable std::mutex _relocated_fields_lock;
};

// Attached-entry spelling used by existing consumers.
using ForwardingEntries = ZForwarding;

} // namespace MapleRuntime

#include "Heap/z/zForwarding.inline.hpp"

#endif // MRT_Z_FORWARDING_H
