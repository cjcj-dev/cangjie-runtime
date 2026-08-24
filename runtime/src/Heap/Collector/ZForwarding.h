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
#include <new>
#include <unordered_map>

#include "Heap/Allocator/ForwardingEntry.h"
#include "Heap/Allocator/ZAttachedArray.h"
#include "Heap/Collector/ZForwardingLife.h"
#include "Heap/Collector/RegionLifeClock.h"

namespace MapleRuntime {

class RegionInfo;

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
        MAddress address;
        bool installed;
    };

    static uint32_t nentries(uint32_t liveObjects)
    {
        // zForwarding.inline.hpp:43-50
        uint32_t n = liveObjects < 1 ? 2 : liveObjects * 2;
        if (n > 1 && (n & (n - 1)) == 0) {
            return n;
        }
        uint32_t p = 1;
        while (p < n) {
            p <<= 1;
        }
        return p;
    }

    static ZForwarding* alloc(uint32_t liveObjects, MAddress start, MAddress heapBase, size_t regionSize,
                              RegionInfo* page, RegionLifeId pageLifeId = 0, bool provisional = false)
    {
        const uint32_t n = nentries(liveObjects);
        void* const addr = AttachedArray::alloc(n);
        if (addr == nullptr) {
            return nullptr;
        }
        return ::new (addr) ZForwarding(page, start, heapBase, regionSize, n, pageLifeId, provisional);
    }

    // Kept name so existing tests / ClearEntries continue to compile.
    static ZForwarding* Create(uint32_t liveObjects, MAddress start, MAddress heapBase, size_t regionSize = 0)
    {
        return alloc(liveObjects, start, heapBase, regionSize, nullptr);
    }

    void Destroy()
    {
        this->~ZForwarding();
        AttachedArray::free(this);
    }

    MAddress start() const { return _start; }
    size_t size() const { return _size; }
    size_t regionSize() const { return _size; }
    RegionInfo* page() { return _page; }
    RegionLifeId page_life_id() const { return _page_life_id; }
    uint64_t publication_generation() const { return _publication_generation; }
    void set_publication_generation(uint64_t generation) { _publication_generation = generation; }
    bool page_life_current(RegionLifeClock::Carrier carrier) const;
    uint32_t length() const { return static_cast<uint32_t>(_entries.length()); }
    bool is_provisional() const { return _provisional; }

    // zPage.inline.hpp:176-185 seqnum bounds livemap/forwarding to one page life.
    // Record the to-region start+regionLifeSeq at insert; consume rejects when
    // InitRegionInfo has bumped that seq (RegionInfo.h:InitRegionInfo).
    void note_to_life(MAddress to);
    static bool DestUsable(MAddress to);
    MAddress resolve_live(MAddress to) const;
    bool receipt_live(MAddress to) const;
    void note_kept_expire() { _kept_seen_expire = true; }
    bool kept_seen_expire() const { return _kept_seen_expire; }
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

    size_t insert(uintptr_t fromIndex, size_t toOffset, ForwardingCursor* cursor, bool* installed = nullptr)
    {
        const ForwardingEntry neu(fromIndex, toOffset);
        std::atomic_thread_fence(std::memory_order_release);
        auto* words = entries();
        for (size_t attempt = 0; attempt < _entries.length(); ++attempt) {
            uint64_t expected = 0;
            if (words[*cursor].compare_exchange_strong(expected, neu.raw(), std::memory_order_relaxed,
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
                return Receipt{ _heapBase + static_cast<MAddress>(existing.to_offset()), false };
            }
            if (beforeFirstCas) {
                beforeFirstCas();
            }
            bool installed = false;
            const size_t finalOff = insert(fromIndex, toOffset, &cursor, &installed);
            if (finalOff != kNotStored) {
                return Receipt{ _heapBase + static_cast<MAddress>(finalOff), installed };
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
                return Receipt{ _heapBase + static_cast<MAddress>(existing.to_offset()), false };
            }
        }
        auto inserted = _overflow.emplace(from, to);
        return Receipt{ inserted.first->second, inserted.second };
    }

    MAddress insert(MAddress from, MAddress to)
    {
        return insert_receipt(from, to).address;
    }

    // zForwarding.cpp:51-53 / :86-194. Dual-inited; product still uses RegionInfo copies.
    bool claim() { return ZForwardingLife::claim(_claimed); }
    bool retain_page() { return ZForwardingLife::retain_page(_ref_count, _done); }
    void release_page() { ZForwardingLife::release_page(_ref_count); }
    void detach_page() { ZForwardingLife::detach_page(_ref_count); }
    void mark_done() { ZForwardingLife::mark_done(_done); }
    bool is_done() const { return ZForwardingLife::is_done(_done); }
    void in_place_relocation_claim_page() { ZForwardingLife::in_place_relocation_claim_page(_ref_count); }

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
          _claimed(false),
          _ref_lock(),
          _ref_count(1),
          _done(false),
          _overflowLock(),
          _overflow(),
          _to_life_n(0),
          _kept_seen_expire(false),
          _provisional(provisional)
    {
        _to_lives[0] = ToLife{};
        _to_lives[1] = ToLife{};
        _to_lives[2] = ToLife{};
    }

    const MAddress _start;
    const size_t _size;
    const MAddress _heapBase;
    const AttachedArray _entries;
    RegionInfo* const _page;
    const RegionLifeId _page_life_id;
    // Monotonic per-region-span generation. Written before the table pointer is
    // published, then immutable for the table's lifetime.
    uint64_t _publication_generation;
    std::atomic<bool> _claimed;
    mutable std::mutex _ref_lock;
    std::atomic<int32_t> _ref_count;
    std::atomic<bool> _done;
    mutable std::mutex _overflowLock;
    std::unordered_map<MAddress, MAddress> _overflow;
    struct ToLife {
        MAddress start;
        uint8_t legacySeq;
        RegionLifeId lifeId;
    };
    ToLife _to_lives[3];
    uint8_t _to_life_n;
    bool _kept_seen_expire;
    const bool _provisional;
};

// Existing tests and ClearEntries still spell this name.
using ForwardingEntries = ZForwarding;

} // namespace MapleRuntime

#endif // MRT_Z_FORWARDING_H
