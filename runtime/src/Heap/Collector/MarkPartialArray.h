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
#include "Heap/Collector/MarkStackEntry.h"

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
// Default ON; MRT_GCV2_PARTIAL_ARRAY=0 keeps the complete inline control path.
namespace MarkPartialArray {

// zGlobals.hpp:82-84. MIN_LENGTH is in elements; our ref slots are 8 bytes,
// same as ZGC's oopSize with compressed oops off.
constexpr size_t MIN_SIZE_SHIFT = 12; // 4K
constexpr size_t MIN_SIZE = static_cast<size_t>(1) << MIN_SIZE_SHIFT;
constexpr size_t MIN_LENGTH = MIN_SIZE / sizeof(MAddress);

// Partial-array payload bounds come from the typed MarkStackEntry layout.
constexpr size_t MAX_LENGTH = static_cast<size_t>(MarkStackEntry::MAX_PARTIAL_ARRAY_LENGTH);
constexpr size_t MAX_OFFSET = static_cast<size_t>(MarkStackEntry::MAX_PARTIAL_ARRAY_OFFSET);

bool Enabled();

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
    return entry.partialArray();
}

// Encodable() must hold before Encode(). A heap wider than
// MAX_OFFSET * MIN_SIZE, or an array longer than MAX_LENGTH, cannot be
// expressed in one word; callers then trace the array inline as before.
bool Encodable(const void* chunkStart, size_t length);
MarkStackEntry Encode(const void* chunkStart, size_t length, bool finalizable = false);
void Decode(const MarkStackEntry& entry, MAddress& chunkStart, size_t& length);

// Positive control: shows whether chunking actually happened.
void NoteArraySplit();
void NoteChunkPushed();
void NoteChunkFollowed();
void NoteNotEncodable();
void Report(const char* point);

} // namespace MarkPartialArray
} // namespace MapleRuntime

#endif // MRT_MARK_PARTIAL_ARRAY_H
