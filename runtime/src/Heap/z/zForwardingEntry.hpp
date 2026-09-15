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

// ZForwardingEntry packs a populated bit and the from/to indices in one CAS word.
// This port uses 23 from-index bits for its 64 MiB regions and 40 heap-offset
// bits for the supported heap address range; there is no parallel mapping.
class ForwardingEntry {
public:
    static constexpr size_t kPopulatedBits = 1;
    static constexpr size_t kToOffsetBits = 40;
    static constexpr size_t kFromIndexBits = 23;
    static constexpr size_t kToOffsetShift = kPopulatedBits;
    static constexpr size_t kFromIndexShift = kPopulatedBits + kToOffsetBits;
    static constexpr uint64_t kToOffsetMask = (1ULL << kToOffsetBits) - 1;
    static constexpr uint64_t kFromIndexMask = (1ULL << kFromIndexBits) - 1;
    static constexpr size_t kMaxFromIndex = static_cast<size_t>(kFromIndexMask);
    static constexpr size_t kMaxToOffset = static_cast<size_t>(kToOffsetMask);
    static_assert(kPopulatedBits + kToOffsetBits + kFromIndexBits == 64, "ForwardingEntry packing");

    ForwardingEntry() : entry_(0) {}
    ForwardingEntry(size_t fromIndex, size_t toOffset)
        : entry_((1ULL << 0) | ((static_cast<uint64_t>(toOffset) & kToOffsetMask) << kToOffsetShift) |
                 ((static_cast<uint64_t>(fromIndex) & kFromIndexMask) << kFromIndexShift))
    {
    }

    bool populated() const { return (entry_ & 1ULL) != 0; }
    size_t to_offset() const { return static_cast<size_t>((entry_ >> kToOffsetShift) & kToOffsetMask); }
    size_t from_index() const { return static_cast<size_t>((entry_ >> kFromIndexShift) & kFromIndexMask); }
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

} // namespace MapleRuntime

#endif // MRT_FORWARDING_ENTRY_H
