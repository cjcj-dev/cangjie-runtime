// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FORWARDING_ENTRY_H
#define MRT_FORWARDING_ENTRY_H

#include <cstddef>
#include <cstdint>

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

} // namespace MapleRuntime

#endif // MRT_FORWARDING_ENTRY_H
