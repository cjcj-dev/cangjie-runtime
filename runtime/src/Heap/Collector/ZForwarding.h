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

#include "Heap/Allocator/ForwardingEntry.h"
#include "Heap/Allocator/ForwardingAllocator.h"
#include "Heap/Allocator/ZAttachedArray.h"
#include "Heap/Collector/ZForwardingLife.h"

namespace MapleRuntime {

class RegionInfo;
class LiveInfo;

// zForwarding.hpp:44-110 — one off-heap object per relocated page.
// _entries is a ZAttachedArray sitting after this object (zAttachedArray.inline.hpp:44-54).
// Lifetime: _ref_count / _ref_lock / _done (zForwarding.cpp:34-194). Product retain/release
// still runs on RegionInfo's copies (step ③); these fields are dual-inited at alloc.
class ZForwarding {
public:
    using AttachedArray = ZAttachedArray<ZForwarding, std::atomic<uint64_t>>;
    static constexpr uint32_t kAlignShift = 3;
    static constexpr size_t kNotStored = SIZE_MAX;

    struct Receipt {
        enum class Status : uint8_t {
            INSTALLED,
            EXISTING,
            DESTINATION_UNTRACKED,
            LIFE_REGISTRY_FULL,
            DESTINATION_LIFE_CONFLICT,
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
        uint64_t liveByteCount = 0;
        uint8_t owner = 1;
        uint8_t largeMarked = 0;
        RegionLifeId lifeId = 0;
    };

    static size_t nentries(size_t objectCountUpperBound)
    {
        // zForwarding.inline.hpp:44-50: power-of-two capacity, at most half full.
        // size_t arithmetic also covers counts above the old uint32_t doubling limit.
        const size_t maxPowerOfTwo = size_t(1) << (std::numeric_limits<size_t>::digits - 1);
        if (objectCountUpperBound > maxPowerOfTwo / 2) {
            return 0;
        }
        const size_t required = objectCountUpperBound == 0 ? 2 : objectCountUpperBound * 2;
        size_t capacity = 2;
        while (capacity < required) {
            capacity <<= 1;
        }
        return capacity;
    }

    static ZForwarding* alloc(size_t liveObjects, MAddress start, MAddress heapBase, size_t regionSize,
                              RegionInfo* page, RegionLifeId pageLifeId = 0, bool provisional = false,
                              const std::shared_ptr<ForwardingAllocator>& arena = {})
    {
        const size_t n = nentries(liveObjects);
        if (n == 0) {
            return nullptr;
        }
        size_t size;
        if (!AttachedArray::allocation_size(n, &size)) {
            return nullptr;
        }
        void* const addr = arena ? arena->allocate(size) : AttachedArray::alloc(n);
        if (addr == nullptr) {
            return nullptr;
        }
        if (arena) {
            AttachedArray::initialize(addr, n);
        }
        auto* forwarding = ::new (addr) ZForwarding(page, start, heapBase, regionSize, n, pageLifeId, provisional);
        forwarding->_arena = arena;
        return forwarding;
    }

    // Kept name so existing tests / ClearEntries continue to compile.
    static ZForwarding* Create(size_t liveObjects, MAddress start, MAddress heapBase, size_t regionSize = 0)
    {
        return alloc(liveObjects, start, heapBase, regionSize, nullptr);
    }

    void Destroy()
    {
        // Hold the arena across destruction of its last embedded carrier.
        auto arena = _arena;
        this->~ZForwarding();
        if (!arena) {
            AttachedArray::free(this);
        }
    }

#if defined(MRT_TESTABLE_INTERNALS)
    const ForwardingAllocator* arena_for_test() const { return _arena.get(); }
#endif

    MAddress start() const { return _start; }
    size_t size() const { return _size; }
    size_t regionSize() const { return _size; }
    RegionInfo* page() const { return _page; }
    RegionLifeId page_life_id() const { return _page_life_id; }
    uint64_t publication_generation() const { return _publication_generation; }
    void set_publication_generation(uint64_t generation) { _publication_generation = generation; }
    uint8_t table_generation() const { return _table_generation; }
    uint64_t birth_flip() const { return _birth_flip; }
    uint64_t required_mark_epoch() const { return _required_mark_epoch; }
    void note_table_epoch(uint8_t generation, uint64_t birthFlip, uint64_t requiredMarkEpoch)
    {
        _table_generation = generation;
        _birth_flip = birthFlip;
        _required_mark_epoch = requiredMarkEpoch;
    }
    bool page_life_current() const;
    size_t length() const { return _entries.length(); }
    bool is_provisional() const { return _provisional; }

    void publish_from_page_view(LiveInfo* liveInfo, uint64_t epoch, MAddress topAtStart,
                                MAddress markStartAllocPtr, uint64_t liveByteCount,
                                uint8_t owner, uint8_t largeMarked, RegionLifeId lifeId)
    {
        // lifeId is the publication word. Readers either reject the zero word
        // or acquire the complete immutable replacement.
        __atomic_store_n(&_from_page.lifeId, static_cast<RegionLifeId>(0), __ATOMIC_RELEASE);
        _from_page.liveInfo = liveInfo;
        _from_page.epoch = epoch;
        _from_page.topAtStart = topAtStart;
        _from_page.markStartAllocPtr = markStartAllocPtr;
        _from_page.liveByteCount = liveByteCount;
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
    MAddress resolve_live(MAddress to) const;
    bool receipt_live(MAddress to) const;

    void note_retired_required() { _retired_required.store(true, std::memory_order_release); }
    bool retired_required() const { return _retired_required.load(std::memory_order_acquire); }
    static std::atomic<uint64_t>& StaleToLifeCount();

    bool covers(MAddress addr) const { return _size != 0 && addr >= _start && addr < _start + _size; }

    uintptr_t index(MAddress from) const { return static_cast<uintptr_t>((from - _start) >> kAlignShift); }

    std::atomic<uint64_t>* entries() const { return _entries(this); }

    ForwardingEntry at(ForwardingCursor* cursor) const
    {
        // zForwarding.inline.hpp:207-211 load-acquire
        return ForwardingEntry::FromRaw(entries()[*cursor].load(std::memory_order_acquire));
    }

    ForwardingEntry first(uintptr_t fromIndex, ForwardingCursor* cursor) const
    {
        const size_t mask = _entries.length() - 1;
        *cursor = static_cast<size_t>(ZHashUint32(static_cast<uint32_t>(fromIndex))) & mask;
        return at(cursor);
    }

    ForwardingEntry next(ForwardingCursor* cursor) const
    {
        const size_t mask = _entries.length() - 1;
        *cursor = (*cursor + 1) & mask;
        return at(cursor);
    }

    // zForwarding.inline.hpp:230-245 plus a bound: our nentries estimate can undersize
    // (REPORT-fwdentries). A miss is a state every caller already handles.
    ForwardingEntry find(uintptr_t fromIndex, ForwardingCursor* cursor) const
    {
        ForwardingEntry entry = first(fromIndex, cursor);
        for (size_t probes = 0; probes < _entries.length() && entry.populated(); ++probes) {
            if (entry.from_index() == fromIndex) {
                return entry;
            }
            entry = next(cursor);
        }
        return entry.populated() ? ForwardingEntry() : entry;
    }

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
    MAddress find(MAddress from) const
    {
        const uintptr_t fromIndex = index(from);
        if (fromIndex <= ForwardingEntry::kMaxFromIndex) {
            ForwardingCursor cursor = 0;
            const ForwardingEntry entry = find(fromIndex, &cursor);
            if (entry.populated()) {
                return _heapBase + static_cast<MAddress>(entry.to_offset());
            }
        }
        std::lock_guard<std::mutex> lock(_overflowLock);
        auto found = _overflow.find(from);
        return found == _overflow.end() ? 0 : found->second;
    }

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

    size_t insert(uintptr_t fromIndex, size_t toOffset, ForwardingCursor* cursor, bool* installed = nullptr)
    {
        const ForwardingEntry neu(fromIndex, toOffset);
        std::atomic_thread_fence(std::memory_order_release);
        auto* words = entries();
        for (size_t attempt = 0; attempt < _entries.length(); ++attempt) {
            uint64_t expected = 0;
            if (words[*cursor].compare_exchange_strong(expected, neu.raw(), std::memory_order_release,
                                                       std::memory_order_relaxed)) {
                if (installed != nullptr) {
                    *installed = true;
                }
                return toOffset;
            }
            ForwardingEntry prev = ForwardingEntry::FromRaw(expected);
            if (!prev.populated()) {
                return toOffset;
            }
            ForwardingEntry entry = at(cursor);
            bool full = true;
            for (size_t probes = 0; probes < _entries.length() && entry.populated(); ++probes) {
                if (entry.from_index() == fromIndex) {
                    if (installed != nullptr) {
                        *installed = false;
                    }
                    return entry.to_offset();
                }
                entry = next(cursor);
            }
            if (!entry.populated()) {
                full = false;
            }
            if (full) {
                break;
            }
        }
        // Preserve the pre-existing attached-array refusal diagnostic while
        // separately recording that the total receipt path fell back to the
        // exact-key map.
        FullRefusals().fetch_add(1, std::memory_order_relaxed);
        FullFallbacks().fetch_add(1, std::memory_order_relaxed);
        return kNotStored;
    }

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

    MAddress insert(MAddress from, MAddress to)
    {
        return insert_receipt(from, to).address;
    }

    // Table users are protected separately from source-page users. A released
    // page must not prevent remap from finding its immutable entries
    // (zForwarding.cpp:171-185; zRelocationSet.cpp:191-197).
    // Acquisition is serialized with unlink by ForwardingTable's install lock.
    bool retain_table()
    {
        _table_readers.fetch_add(1, std::memory_order_acquire);
        return true;
    }
    void release_table() { _table_readers.fetch_sub(1, std::memory_order_release); }
    void drain_table_readers()
    {
        _table_draining.store(true, std::memory_order_release);
        while (_table_readers.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
    }
    size_t table_readers() const { return _table_readers.load(std::memory_order_acquire); }
    bool table_draining() const { return _table_draining.load(std::memory_order_acquire); }

    // The region facade and queued page work may outlive map membership. They
    // hold the carrier, not a payload retain or an open publication token.
    void retain_owner() { _external_owners.fetch_add(1, std::memory_order_relaxed); }
    void release_owner() { _external_owners.fetch_sub(1, std::memory_order_release); }
    size_t external_owners() const { return _external_owners.load(std::memory_order_acquire); }

    enum class ZPublishState : int8_t {
        none,
        published,
        reject,
        accept,
    };

    static uint32_t young_seqnum()
    {
        return YoungSeqnum().load(std::memory_order_acquire);
    }
    static void bump_young_seqnum()
    {
        YoungSeqnum().fetch_add(1, std::memory_order_acq_rel);
    }

    void relocated_remembered_fields_register(MAddress field)
    {
        const ZPublishState state = _relocated_remembered_fields_state.load(std::memory_order_relaxed);
        if (state == ZPublishState::reject) {
            return;
        }
        std::lock_guard<std::mutex> lock(_relocated_fields_lock);
        _relocated_remembered_fields_array.push_back(field);
    }

    bool relocated_remembered_fields_is_concurrently_scanned() const
    {
        return _relocated_remembered_fields_state.load(std::memory_order_relaxed) == ZPublishState::reject;
    }

    void relocated_remembered_fields_after_relocate()
    {
        _relocated_remembered_fields_publish_young_seqnum = young_seqnum();
        if (!relocated_remembered_fields_is_concurrently_scanned()) {
            relocated_remembered_fields_publish();
        }
    }

    void relocated_remembered_fields_publish()
    {
        ZPublishState expected = ZPublishState::none;
        if (!_relocated_remembered_fields_state.compare_exchange_strong(
                expected, ZPublishState::published, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(_relocated_fields_lock);
            _relocated_remembered_fields_array.clear();
        }
    }

    void relocated_remembered_fields_notify_concurrent_scan_of()
    {
        ZPublishState expected = ZPublishState::none;
        if (_relocated_remembered_fields_state.compare_exchange_strong(
                expected, ZPublishState::reject, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return;
        }
        if (expected == ZPublishState::published) {
            ZPublishState published = ZPublishState::published;
            if (_relocated_remembered_fields_state.compare_exchange_strong(
                    published, ZPublishState::reject, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> lock(_relocated_fields_lock);
                _relocated_remembered_fields_array.clear();
            }
        }
    }

    template<typename Function>
    void relocated_remembered_fields_apply_to_published(Function function)
    {
        const ZPublishState state = _relocated_remembered_fields_state.load(std::memory_order_acquire);
        if (state == ZPublishState::published) {
            std::lock_guard<std::mutex> lock(_relocated_fields_lock);
            for (MAddress field : _relocated_remembered_fields_array) {
                function(field);
            }
            _relocated_remembered_fields_array.clear();
        }
        if (_relocated_remembered_fields_publish_young_seqnum == young_seqnum()) {
            _relocated_remembered_fields_state.store(ZPublishState::reject, std::memory_order_relaxed);
        } else {
            _relocated_remembered_fields_state.store(ZPublishState::accept, std::memory_order_relaxed);
        }
    }

    // zForwarding.cpp:51-53 / :86-194. Source-page ownership only.
    bool claim() { return ZForwardingLife::claim(_claimed); }
    bool is_claimed() const { return _claimed.load(std::memory_order_acquire); }
    bool in_place() const { return _in_place.load(std::memory_order_acquire); }
    void set_in_place() { _in_place.store(true, std::memory_order_release); }
    bool retain_page() { return ZForwardingLife::retain_page(_ref_count, _done); }
    void release_page()
    {
        int32_t count = _ref_count.load(std::memory_order_relaxed);
        for (;;) {
            CHECK(count != 0);
            const int32_t next = count > 0 ? count - 1 : count + 1;
            if (_ref_count.compare_exchange_weak(count, next, std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
                if (next == 0 || next == -1) {
                    std::lock_guard<std::mutex> lock(_ref_lock);
                    _ref_changed.notify_all();
                }
                return;
            }
        }
    }
    void detach_page()
    {
        std::unique_lock<std::mutex> lock(_ref_lock);
        _ref_changed.wait(lock, [this] { return _ref_count.load(std::memory_order_acquire) == 0; });
    }
    void mark_done() { ZForwardingLife::mark_done(_done); }
    bool is_done() const { return ZForwardingLife::is_done(_done); }
    void in_place_relocation_claim_page()
    {
        int32_t count = _ref_count.load(std::memory_order_relaxed);
        do {
            CHECK(count > 0);
        } while (!_ref_count.compare_exchange_weak(count, -count, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed));
        std::unique_lock<std::mutex> lock(_ref_lock);
        _ref_changed.wait(lock, [this] { return _ref_count.load(std::memory_order_acquire) == -1; });
    }

    std::atomic<int32_t>& ref_count() { return _ref_count; }
    std::atomic<bool>& claimed() { return _claimed; }
    std::atomic<bool>& done() { return _done; }
    std::mutex& ref_lock() const { return _ref_lock; }

private:
    // zForwarding.inline.hpp:59-76
    ZForwarding(RegionInfo* page, MAddress start, MAddress heapBase, size_t regionSize, size_t nentries,
                RegionLifeId pageLifeId, bool provisional)
        : _start(start),
          _size(regionSize),
          _heapBase(heapBase),
          _entries(nentries),
          _page(page),
          _page_life_id(pageLifeId),
          _publication_generation(0),
          _table_generation(0),
          _birth_flip(0),
          _required_mark_epoch(0),
          _claimed(false),
          _in_place(false),
          _ref_lock(),
          _ref_count(1),
          _done(false),
          _overflowLock(),
          _overflow(),
          _receiptInstallLock(),
          _retired_required(false),
          _provisional(provisional),
          _from_page(),
          _relocated_remembered_fields_state(ZPublishState::none),
          _relocated_remembered_fields_publish_young_seqnum(0)
    {}

    std::shared_ptr<ForwardingAllocator> _arena;
    const MAddress _start;
    const size_t _size;
    const MAddress _heapBase;
    const AttachedArray _entries;
    RegionInfo* const _page;
    const RegionLifeId _page_life_id;
    // Monotonic per-region-span generation. Written before the table pointer is
    // published, then immutable for the table's lifetime.
    uint64_t _publication_generation;
    uint8_t _table_generation;
    uint64_t _birth_flip;
    uint64_t _required_mark_epoch;
    std::atomic<bool> _claimed;
    std::atomic<bool> _in_place;
    mutable std::mutex _ref_lock;
    std::condition_variable _ref_changed;
    std::atomic<int32_t> _ref_count;
    std::atomic<bool> _done;
    std::atomic<size_t> _table_readers{ 0 };
    std::atomic<bool> _table_draining{ false };
    std::atomic<size_t> _external_owners{ 0 };
    mutable std::mutex _overflowLock;
    std::unordered_map<MAddress, MAddress> _overflow;
    mutable std::mutex _receiptInstallLock;
    std::atomic<bool> _retired_required;
    const bool _provisional;
    FromPageView _from_page;
    std::atomic<ZPublishState> _relocated_remembered_fields_state;
    std::vector<MAddress> _relocated_remembered_fields_array;
    uint32_t _relocated_remembered_fields_publish_young_seqnum;
    mutable std::mutex _relocated_fields_lock;

    static std::atomic<uint32_t>& YoungSeqnum()
    {
        static std::atomic<uint32_t> seq{ 1 };
        return seq;
    }
};

// Existing tests and ClearEntries still spell this name.
using ForwardingEntries = ZForwarding;

} // namespace MapleRuntime

#endif // MRT_Z_FORWARDING_H
