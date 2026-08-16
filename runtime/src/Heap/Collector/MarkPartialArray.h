// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MARK_PARTIAL_ARRAY_H
#define MRT_MARK_PARTIAL_ARRAY_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

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
// Why: our mark work stack only forks between whole objects --
// ConcurrentMarkingWork::TryForkTask runs after TraceObjectRefFields has
// returned -- so one large array is traced start-to-end by a single thread.
// ZGC splits the array and pushes the remainder back onto the mark stack,
// which makes the tail stealable by the other mark workers.
//
// Default OFF; MRT_GCV2_PARTIAL_ARRAY=1 opts in.
namespace MarkPartialArray {

// zGlobals.hpp:82-84. MIN_LENGTH is in elements; our ref slots are 8 bytes,
// same as ZGC's oopSize with compressed oops off.
constexpr size_t MIN_SIZE_SHIFT = 12; // 4K
constexpr size_t MIN_SIZE = static_cast<size_t>(1) << MIN_SIZE_SHIFT;
constexpr size_t MIN_LENGTH = MIN_SIZE / sizeof(MAddress);

// Work-stack entry encoding.
//
// ZGC carries a dedicated tagged entry type (ZMarkStackEntry) on its mark
// stack. Our WorkStack is MarkStack<BaseObject*>, so a chunk has to travel
// inside a BaseObject* slot instead. Ordinary entries are heap object
// pointers and therefore 8-byte aligned, which leaves bit 0 free as the
// discriminator; the remaining fields mirror ZGC's partial-array layout
// (zMarkStackEntry.hpp:56-71) -- a heap-relative address in MIN_SIZE units
// plus an element count.
//
//   bit     0    partial-array tag (1 = chunk, 0 = ordinary object pointer)
//   bits  1-31   chunk length, in elements
//   bits 32-63   chunk start, as (addr - heapStartAddr) >> MIN_SIZE_SHIFT
constexpr uintptr_t TAG_MASK = 1u;
constexpr unsigned LENGTH_SHIFT = 1;
constexpr unsigned LENGTH_BITS = 31;
constexpr unsigned OFFSET_SHIFT = 32;
constexpr unsigned OFFSET_BITS = 32;
constexpr size_t MAX_LENGTH = (static_cast<size_t>(1) << LENGTH_BITS) - 1;
constexpr size_t MAX_OFFSET = (static_cast<size_t>(1) << OFFSET_BITS) - 1;

bool Enabled();

// Hot path: runs on every work-stack pop.
inline bool IsPartialArrayEntry(const BaseObject* entry)
{
    return (reinterpret_cast<uintptr_t>(entry) & TAG_MASK) != 0;
}

// Encodable() must hold before Encode(). A heap wider than
// MAX_OFFSET * MIN_SIZE, or an array longer than MAX_LENGTH, cannot be
// expressed in one word; callers then trace the array inline as before.
bool Encodable(const void* chunkStart, size_t length);
BaseObject* Encode(const void* chunkStart, size_t length);
void Decode(const BaseObject* entry, MAddress& chunkStart, size_t& length);

// Positive control: shows whether chunking actually happened.
void NoteArraySplit();
void NoteChunkPushed();
void NoteChunkFollowed();
void NoteNotEncodable();
void Report(const char* point);

} // namespace MarkPartialArray
} // namespace MapleRuntime

#endif // MRT_MARK_PARTIAL_ARRAY_H
