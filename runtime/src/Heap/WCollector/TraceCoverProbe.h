// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_TRACE_COVER_PROBE_H
#define MRT_TRACE_COVER_PROBE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;

// Default-off probe: MRT_GCV2_TRACE_COVER=1
// Marks ref-field / root-slot addresses visited by major ENUM+TRACE; after Flip,
// postflip fix buckets those rewrites into traced vs untraced (heap vs root).
// Side-channel only — no object-header / RefField layout change.
namespace TraceCoverProbe {
bool Enabled();
void BeginMajorCycle();
void MarkSlot(const void* slotAddr);
void MigrateObject(const BaseObject* fromObj, const BaseObject* toObj, size_t size);
// kind: 0=heap field, 1=root slot. only count when a fix rewrite occurred.
void AccountFixed(const void* slotAddr, int kind);
void ReportPostflip();
void NotePostflipFixedBaseline(size_t fixedHeap, size_t fixedRoot);
} // namespace TraceCoverProbe
} // namespace MapleRuntime

#endif // MRT_TRACE_COVER_PROBE_H
