// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FORWARDING_ENTRY_H
#define MRT_FORWARDING_ENTRY_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Common/TypeDef.h"

namespace MapleRuntime {

// zForwardingEntry.hpp:32-47 — populated:1 | to_offset:45 | from_index:18
//
// 18 bits of from_index times the 8-byte object alignment is exactly 2 MB, which is ZGC's small
// page size -- the field is sized to its container, not chosen loosely.  Our regions are not
// bounded by 2 MB, so the same field silently wraps on a larger one: two objects whose indices
// differ by 2^18 compare equal and find() hands back the wrong to-address.  A miss would be
// harmless (the caller falls back to route geometry); a wrong destination is not.  kMaxFromIndex
// exists so that case is refused at insert time instead.
class ForwardingEntry {
public:
    static constexpr size_t kFromIndexBits = 18;
    static constexpr size_t kMaxFromIndex = (size_t(1) << kFromIndexBits) - 1;

    ForwardingEntry() : entry_(0) {}
    ForwardingEntry(size_t fromIndex, size_t toOffset)
        : entry_((1ULL << 0) | ((static_cast<uint64_t>(toOffset) & ((1ULL << 45) - 1)) << 1) |
                 ((static_cast<uint64_t>(fromIndex) & ((1ULL << 18) - 1)) << 46))
    {
    }

    bool populated() const { return (entry_ & 1ULL) != 0; }
    size_t to_offset() const { return static_cast<size_t>((entry_ >> 1) & ((1ULL << 45) - 1)); }
    size_t from_index() const { return static_cast<size_t>((entry_ >> 46) & ((1ULL << 18) - 1)); }
    uint64_t raw() const { return entry_; }
    static ForwardingEntry FromRaw(uint64_t raw)
    {
        ForwardingEntry e;
        e.entry_ = raw;
        return e;
    }

private:
    uint64_t entry_;
};

using ForwardingCursor = size_t;

// zHash.inline.hpp:63-70
inline uint32_t ZHashUint32(uint32_t key)
{
    key = ~key + (key << 15);
    key = key ^ (key >> 12);
    key = key + (key << 2);
    key = key ^ (key >> 4);
    key = key * 2057;
    key = key ^ (key >> 16);
    return key;
}

// Per-page from→to table. zForwarding.inline.hpp:43-50 nentries, :226-304 find/insert.
class ForwardingEntries {
public:
    static constexpr uint32_t kAlignShift = 3;

    static uint32_t NEntries(uint32_t liveObjects)
    {
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

    static ForwardingEntries* Create(uint32_t liveObjects, MAddress start, MAddress heapBase)
    {
        const uint32_t n = NEntries(liveObjects);
        auto* self = static_cast<ForwardingEntries*>(std::malloc(sizeof(ForwardingEntries)));
        if (self == nullptr) {
            return nullptr;
        }
        self->length_ = n;
        self->start_ = start;
        self->heapBase_ = heapBase;
        self->words_ = static_cast<std::atomic<uint64_t>*>(std::calloc(n, sizeof(std::atomic<uint64_t>)));
        if (self->words_ == nullptr) {
            std::free(self);
            return nullptr;
        }
        return self;
    }

    void Destroy()
    {
        std::free(words_);
        words_ = nullptr;
        std::free(this);
    }

    uint32_t length() const { return length_; }
    MAddress start() const { return start_; }

    uintptr_t index(MAddress from) const { return static_cast<uintptr_t>((from - start_) >> kAlignShift); }

    ForwardingEntry at(ForwardingCursor* cursor) const
    {
        return ForwardingEntry::FromRaw(words_[*cursor].load(std::memory_order_acquire));
    }

    ForwardingEntry first(uintptr_t fromIndex, ForwardingCursor* cursor) const
    {
        const size_t mask = length_ - 1;
        *cursor = static_cast<size_t>(ZHashUint32(static_cast<uint32_t>(fromIndex))) & mask;
        return at(cursor);
    }

    ForwardingEntry next(ForwardingCursor* cursor) const
    {
        const size_t mask = length_ - 1;
        *cursor = (*cursor + 1) & mask;
        return at(cursor);
    }

    // zForwarding.inline.hpp:230-245, plus a bound ZGC does not need.
    //
    // ZGC's loop is `while (entry.populated())` with no trip count, and that is safe there because
    // the table is never full: nentries is next_pow2(live_objects * 2), so the load factor cannot
    // exceed one half and an empty slot always terminates the probe. Ours is sized from an estimate
    // that can come out far too small, and on a full table this loop never ends -- observed as two
    // GC threads at 100% CPU inside find(), a collection that never finishes and a workload that
    // went from 10 seconds to a 300-second timeout.
    //
    // Bounding it turns "hang" into "miss", and a miss is a state every caller already handles:
    // FindToVersion falls back to route geometry, which is what it did before this table existed.
    ForwardingEntry find(uintptr_t fromIndex, ForwardingCursor* cursor) const
    {
        ForwardingEntry entry = first(fromIndex, cursor);
        for (size_t probes = 0; probes < length_ && entry.populated(); ++probes) {
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
        ForwardingCursor cursor = 0;
        const ForwardingEntry entry = find(index(from), &cursor);
        if (!entry.populated()) {
            return 0;
        }
        return heapBase_ + static_cast<MAddress>(entry.to_offset());
    }

    // zForwarding.inline.hpp:267-292, bounded for the same reason find() is: on a full table the
    // rescan loop below never finds an empty slot, and the outer retry never stops asking for one.
    // kNotStored says so out loud instead of spinning.
    static constexpr size_t kNotStored = SIZE_MAX;

    size_t insert(uintptr_t fromIndex, size_t toOffset, ForwardingCursor* cursor)
    {
        const ForwardingEntry neu(fromIndex, toOffset);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t attempt = 0; attempt < length_; ++attempt) {
            uint64_t expected = 0;
            if (words_[*cursor].compare_exchange_strong(expected, neu.raw(), std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
                return toOffset;
            }
            ForwardingEntry prev = ForwardingEntry::FromRaw(expected);
            if (!prev.populated()) {
                return toOffset;
            }
            ForwardingEntry entry = at(cursor);
            bool full = true;
            for (size_t probes = 0; probes < length_ && entry.populated(); ++probes) {
                if (entry.from_index() == fromIndex) {
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
        FullRefusals().fetch_add(1, std::memory_order_relaxed);
        return kNotStored;
    }

    // Inserts dropped because the table had no free slot. Separate from OverflowRefusals: that one
    // means "this region is too big for an 18-bit index", this one means "we sized the table too
    // small". Both degrade to a geometry fallback, and both are invisible without a counter.
    static std::atomic<uint64_t>& FullRefusals()
    {
        static std::atomic<uint64_t> n{ 0 };
        return n;
    }

    // Number of insert() calls refused because the region is larger than one from_index can
    // address. Product code treats the refusal as "no entry" and falls back to geometry, so this
    // counter is the only way to tell "the table never had it" from "the table declined to store
    // it" -- and a silent zero here is what a truncating build would look like from the outside.
    static std::atomic<uint64_t>& OverflowRefusals()
    {
        static std::atomic<uint64_t> n{ 0 };
        return n;
    }

    MAddress insert(MAddress from, MAddress to)
    {
        ForwardingCursor cursor = 0;
        const uintptr_t fromIndex = index(from);
        if (fromIndex > ForwardingEntry::kMaxFromIndex) {
            OverflowRefusals().fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        (void)find(fromIndex, &cursor);
        const size_t toOffset = static_cast<size_t>(to - heapBase_);
        const size_t finalOff = insert(fromIndex, toOffset, &cursor);
        if (finalOff == kNotStored) {
            return 0;
        }
        return heapBase_ + static_cast<MAddress>(finalOff);
    }

private:
    uint32_t length_ = 0;
    MAddress start_ = 0;
    MAddress heapBase_ = 0;
    std::atomic<uint64_t>* words_ = nullptr;
};

} // namespace MapleRuntime

#endif // MRT_FORWARDING_ENTRY_H
