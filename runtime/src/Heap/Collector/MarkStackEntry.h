// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MARK_STACK_ENTRY_H
#define MRT_MARK_STACK_ENTRY_H

#include <cstddef>
#include <cstdint>

#include "Base/Log.h"

namespace MapleRuntime {
class BaseObject;

// One-word, typed mark-stack entry. The layout and independent policy flags
// follow OpenJDK ZMarkStackEntry (zMarkStackEntry.hpp:56-121):
//
// Object: bits 63..5 address, 4 mark, 3 inc-live, 2 follow,
//         1 partial-array (zero), 0 finalizable.
// Partial array: bits 63..32 heap-relative 4K offset, 31..2 length,
//                1 partial-array (one), 0 finalizable.
//
// The previous stack element type was BaseObject*. Partial-array work therefore
// forged a tagged BaseObject* and relied on every consumer checking bit zero
// before object/region access. This type makes the two alternatives disjoint;
// consumers must explicitly select object() or the partial-array accessors.
class MarkStackEntry {
public:
    static constexpr unsigned FINALIZABLE_SHIFT = 0;
    static constexpr unsigned PARTIAL_ARRAY_SHIFT = 1;
    static constexpr unsigned FOLLOW_SHIFT = 2;
    static constexpr unsigned INC_LIVE_SHIFT = 3;
    static constexpr unsigned MARK_SHIFT = 4;
    static constexpr unsigned OBJECT_ADDRESS_SHIFT = 5;
    static constexpr unsigned OBJECT_ADDRESS_BITS = 59;
    static constexpr unsigned PARTIAL_ARRAY_LENGTH_SHIFT = 2;
    static constexpr unsigned PARTIAL_ARRAY_LENGTH_BITS = 30;
    static constexpr unsigned PARTIAL_ARRAY_OFFSET_SHIFT = 32;
    static constexpr unsigned PARTIAL_ARRAY_OFFSET_BITS = 32;

    static constexpr uint64_t MAX_OBJECT_ADDRESS =
        (static_cast<uint64_t>(1) << OBJECT_ADDRESS_BITS) - 1;
    static constexpr uint64_t MAX_PARTIAL_ARRAY_LENGTH =
        (static_cast<uint64_t>(1) << PARTIAL_ARRAY_LENGTH_BITS) - 1;
    static constexpr uint64_t MAX_PARTIAL_ARRAY_OFFSET =
        (static_cast<uint64_t>(1) << PARTIAL_ARRAY_OFFSET_BITS) - 1;

    // Like ZMarkStackEntry, do not initialize the backing word: every new
    // MarkStackBuf contains 64 entries and only its populated prefix is read.
    MarkStackEntry() {}

    // Ordinary pushes retain the old work-stack meaning. Deliberately implicit
    // so generic producers can push BaseObject*, while consumers still have to
    // decode the typed alternative explicitly.
    MarkStackEntry(BaseObject* object)
        : MarkStackEntry(object, true, true, true, false)
    {}

    MarkStackEntry(BaseObject* object, bool mark, bool incLive, bool follow, bool finalizable)
        : entry((CheckedObjectAddress(object) << OBJECT_ADDRESS_SHIFT) |
                (static_cast<uint64_t>(mark) << MARK_SHIFT) |
                (static_cast<uint64_t>(incLive) << INC_LIVE_SHIFT) |
                (static_cast<uint64_t>(follow) << FOLLOW_SHIFT) |
                (static_cast<uint64_t>(finalizable) << FINALIZABLE_SHIFT))
    {}

    static MarkStackEntry MarkOnly(BaseObject* object, bool finalizable = false)
    {
        return MarkStackEntry(object, true, true, false, finalizable);
    }

    static MarkStackEntry FollowOnly(BaseObject* object, bool finalizable = false)
    {
        return MarkStackEntry(object, false, false, true, finalizable);
    }

    static MarkStackEntry MarkAndFollow(BaseObject* object, bool finalizable = false)
    {
        return MarkStackEntry(object, true, true, true, finalizable);
    }

    static MarkStackEntry PartialArray(size_t offset, size_t length, bool finalizable = false)
    {
        CHECK_DETAIL(offset <= MAX_PARTIAL_ARRAY_OFFSET,
                     "partial-array mark entry offset does not fit: %zu", offset);
        CHECK_DETAIL(length != 0 && length <= MAX_PARTIAL_ARRAY_LENGTH,
                     "partial-array mark entry length does not fit: %zu", length);
        return MarkStackEntry((static_cast<uint64_t>(offset) << PARTIAL_ARRAY_OFFSET_SHIFT) |
                              (static_cast<uint64_t>(length) << PARTIAL_ARRAY_LENGTH_SHIFT) |
                              (static_cast<uint64_t>(1) << PARTIAL_ARRAY_SHIFT) |
                              (static_cast<uint64_t>(finalizable) << FINALIZABLE_SHIFT));
    }

    bool finalizable() const { return Bit(FINALIZABLE_SHIFT); }
    bool partialArray() const { return Bit(PARTIAL_ARRAY_SHIFT); }
    bool follow() const { return Bit(FOLLOW_SHIFT); }
    bool incLive() const { return Bit(INC_LIVE_SHIFT); }
    bool mark() const { return Bit(MARK_SHIFT); }

    BaseObject* object() const
    {
        CHECK_DETAIL(!partialArray(), "partial-array mark entry used as an object");
        return reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(entry >> OBJECT_ADDRESS_SHIFT));
    }

    size_t partialArrayOffset() const
    {
        CHECK_DETAIL(partialArray(), "object mark entry used as a partial array");
        return static_cast<size_t>(entry >> PARTIAL_ARRAY_OFFSET_SHIFT);
    }

    size_t partialArrayLength() const
    {
        CHECK_DETAIL(partialArray(), "object mark entry used as a partial array");
        return static_cast<size_t>((entry >> PARTIAL_ARRAY_LENGTH_SHIFT) & MAX_PARTIAL_ARRAY_LENGTH);
    }

    static bool IsObjectAddressEncodable(const BaseObject* object)
    {
        return reinterpret_cast<uintptr_t>(object) <= MAX_OBJECT_ADDRESS;
    }

private:
    static uint64_t CheckedObjectAddress(const BaseObject* object)
    {
        const uintptr_t address = reinterpret_cast<uintptr_t>(object);
        CHECK_DETAIL(static_cast<uint64_t>(address) <= MAX_OBJECT_ADDRESS,
                     "object address does not fit mark entry: %p", static_cast<const void*>(object));
        return static_cast<uint64_t>(address);
    }

    explicit MarkStackEntry(uint64_t raw) : entry(raw) {}

    bool Bit(unsigned shift) const { return ((entry >> shift) & 1u) != 0; }

    uint64_t entry;
};

static_assert(sizeof(MarkStackEntry) == sizeof(uint64_t), "mark stack entry must stay one word");

} // namespace MapleRuntime

#endif // MRT_MARK_STACK_ENTRY_H
