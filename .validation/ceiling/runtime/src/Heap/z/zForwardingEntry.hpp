// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_FORWARDING_ENTRY_H
#define MRT_FORWARDING_ENTRY_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {

class ZForwardingEntry {
public:
    using field_populated = uint64_t;
    static constexpr size_t kPopulatedBits = 1;
    static constexpr size_t kToOffsetBits = 45;
    static constexpr size_t kFromIndexBits = 18;
    static constexpr size_t kToOffsetShift = 1;
    static constexpr size_t kFromIndexShift = 46;
    static constexpr uint64_t kToOffsetMask = (1ULL << kToOffsetBits) - 1;
    static constexpr uint64_t kFromIndexMask = (1ULL << kFromIndexBits) - 1;
    static constexpr size_t kMaxFromIndex = static_cast<size_t>(kFromIndexMask);
    static constexpr size_t kMaxToOffset = static_cast<size_t>(kToOffsetMask);
    static_assert(kPopulatedBits + kToOffsetBits + kFromIndexBits == 64, "ZForwardingEntry 1/45/18");

    ZForwardingEntry() : _entry(0) {}
    ZForwardingEntry(size_t from_index, size_t to_offset)
        : _entry(1ULL | ((static_cast<uint64_t>(to_offset) & kToOffsetMask) << kToOffsetShift) |
                 ((static_cast<uint64_t>(from_index) & kFromIndexMask) << kFromIndexShift))
    {
    }

    bool populated() const { return (_entry & 1ULL) != 0; }
    size_t to_offset() const { return static_cast<size_t>((_entry >> kToOffsetShift) & kToOffsetMask); }
    size_t from_index() const { return static_cast<size_t>((_entry >> kFromIndexShift) & kFromIndexMask); }
    uint64_t raw() const { return _entry; }
    static ZForwardingEntry FromRaw(uint64_t raw)
    {
        ZForwardingEntry e;
        e._entry = raw;
        return e;
    }

private:
    uint64_t _entry;
};

using ForwardingEntry = ZForwardingEntry;
using ForwardingCursor = size_t;
using ZForwardingCursor = size_t;

} // namespace MapleRuntime

#endif
