// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_SLOT_WRITER_PROBE_H
#define MRT_HEAP_SLOT_WRITER_PROBE_H

#include "Base/Types.h"
#include "Common/BaseObject.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {

// Answers T1 of lane slotwriter: when a heap ref field was written, was the
// stored value a valid managed object at that instant?
// Gate (default off): MRT_GCV2_SLOTWRITER=1
// Dump cap: MRT_GCV2_SLOTWRITER_DUMP_MAX=<N> (default 32)
//
// Records last write per slot (and a global ring) from mutator barrier write
// paths. On invalid minor enqueue of a closure_edge, dumps the last write(s)
// that targeted that slot / value.
class SlotWriterProbe {
public:
    static bool Enabled();

    // Called from Barrier write entry points after the store is visible.
    // path: "WriteReference" | "AtomicWrite" | "AtomicSwap" | "CAS" | "WriteStruct" | "gc_cas"
    static void NoteRefWrite(BaseObject* holder, MAddress slot, BaseObject* value, const char* path);

    // Called when minor enqueue sees an invalid object from a known slot.
    static void OnInvalidEnqueue(BaseObject* object, BaseObject* holder, MAddress slot, MAddress raw,
                                 const char* origin);

    // Optional: dump ring stats at process end / abort site.
    static void FlushSummary(const char* site);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_SLOT_WRITER_PROBE_H
