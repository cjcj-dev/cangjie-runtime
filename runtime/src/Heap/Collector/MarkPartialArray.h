// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MARK_PARTIAL_ARRAY_H
#define MRT_MARK_PARTIAL_ARRAY_H

#include <cstddef>
#include <cstdint>
#include <functional>

#include "Common/TypeDef.h"
#include "Heap/z/zMarkStackEntry.hpp"
#include "Heap/z/zGlobals.hpp"

namespace MapleRuntime {
class BaseObject;

// Large reference-array chunking for the mark phase.
//
// Ported from ZGC, reference/jdk/src/hotspot/share/gc/z/zMark.cpp:
//   follow_array_elements       :257-263
//   follow_array_elements_small :208-214
//   follow_array_elements_large :216-255
//   push_partial_array          :185-196
// and the constants at reference/jdk/src/hotspot/share/gc/z/zGlobals.hpp:82-84.
//
// Why: without this split, one large array is traced start-to-end by a
// single worker after TraceObjectRefFields returns.
// ZGC splits the array and pushes the remainder back onto the mark stack,
// which makes the tail stealable by the other mark workers.
//
namespace MarkPartialArray {

using FieldVisitor = std::function<void(MAddress)>;
using EntryPublisher = std::function<void(const MarkStackEntry&)>;

// One producer/consumer implementation for both generations. Struct arrays
// retain their GCTib walk; reference arrays publish typed continuations.
void FollowObjectReferences(BaseObject* object, bool finalizable,
                            const FieldVisitor& visit, const EntryPublisher& publish);
void FollowPartialReferences(const MarkStackEntry& entry,
                             const FieldVisitor& visit, const EntryPublisher& publish);
void FollowElements(MAddress start, size_t length, bool finalizable,
                    const FieldVisitor& visit, const EntryPublisher& publish);


// Hot path: runs on every work-stack pop.
inline bool IsPartialArrayEntry(const MarkStackEntry& entry)
{
    return entry.partial_array();
}

// zMark.cpp:177-196: the entry stores ZAddress::offset(chunk) >> MIN_SIZE_SHIFT.
MarkStackEntry Encode(const void* chunkStart, size_t length, bool finalizable = false);
void Decode(const MarkStackEntry& entry, MAddress& chunkStart, size_t& length);

} // namespace MarkPartialArray
} // namespace MapleRuntime

#endif // MRT_MARK_PARTIAL_ARRAY_H
