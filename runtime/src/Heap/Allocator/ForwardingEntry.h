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
class ForwardingEntry {
public:
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

    // zForwarding.inline.hpp:230-245
    ForwardingEntry find(uintptr_t fromIndex, ForwardingCursor* cursor) const
    {
        ForwardingEntry entry = first(fromIndex, cursor);
        while (entry.populated()) {
            if (entry.from_index() == fromIndex) {
                return entry;
            }
            entry = next(cursor);
        }
        return entry;
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

    // zForwarding.inline.hpp:267-292
    size_t insert(uintptr_t fromIndex, size_t toOffset, ForwardingCursor* cursor)
    {
        const ForwardingEntry neu(fromIndex, toOffset);
        std::atomic_thread_fence(std::memory_order_release);
        for (;;) {
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
            while (entry.populated()) {
                if (entry.from_index() == fromIndex) {
                    return entry.to_offset();
                }
                entry = next(cursor);
            }
        }
    }

    MAddress insert(MAddress from, MAddress to)
    {
        ForwardingCursor cursor = 0;
        const uintptr_t fromIndex = index(from);
        (void)find(fromIndex, &cursor);
        const size_t toOffset = static_cast<size_t>(to - heapBase_);
        const size_t finalOff = insert(fromIndex, toOffset, &cursor);
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
