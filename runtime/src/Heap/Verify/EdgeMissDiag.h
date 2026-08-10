// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_EDGE_MISS_DIAG_H
#define MRT_EDGE_MISS_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;

// edgemiss: decide 甲-1 miss-follow vs 甲-2 post-mark-write for
// holder.myData → RawArray soft-nulls (f3why).
//
// Gate (default off; product path early-return BEFORE any work):
//   MRT_GCV2_EDGEMISS=1
//   or MRT_GCV2_DIAG=edgemiss / all
//
// Sites:
//   NoteMark  — first-time MarkObject paint of obj (wasMarked=false)
//   NoteWrite — Enum/Trace barrier ref write (field slot + holder + new/old ref)
//   LookupAtF3 — FixOldTaggedRefField soft-null: stamp write vs mark times
//
// ⛔ diag-only. Do NOT merge to product path.

namespace EdgeMissDiag {

bool Enabled();

// First mark of obj in this GC (caller: MarkObject when !wasMarked).
void NoteMark(BaseObject* obj);

// Mutator/GC barrier write of a ref field.
// site: "enum" | "trace" | "trace_atomic" | "enum_atomic" | ...
// satbOld/satbNew: whether RememberObjectInSatbBuffer was invoked for old/new
//   (1=called, 0=not, 0xff=n/a). Does not prove enqueue survived ShouldEnqueue.
void NoteWrite(BaseObject* holder, void* field, BaseObject* oldRef, BaseObject* newRef, const char* site,
               unsigned satbOld, unsigned satbNew);

// At F3 soft-null: look up stamps for (holder, field, from) and LOG one line.
// Returns verdict tag for optional caller use (never changes control flow).
// verdict: "post_mark_write" | "miss_follow" | "no_write_stamp" | "no_holder_mark" | "off"
const char* LookupAtF3(BaseObject* holder, void* field, BaseObject* from, int holderMarked, int fromMarked);

} // namespace EdgeMissDiag
} // namespace MapleRuntime

#endif // MRT_EDGE_MISS_DIAG_H
