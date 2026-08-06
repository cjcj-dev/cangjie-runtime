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

    // Called from Barrier / GC install paths after the store is visible.
    // path tags (see SlotWriterProbe.cpp PathTag):
    //   WriteReference | AtomicWrite | AtomicSwap | CAS | WriteStruct | WriteStructWord
    //   WriteStatic | CasInstallPlain | FixMinorSlot | TryUpdateRef | TraceTag
    //   FixOldTag | ForwardRoot | EnumTag | UntagRef
    static void NoteRefWrite(BaseObject* holder, MAddress slot, BaseObject* value, const char* path);

    // After WriteStruct memcpy: scan 8-byte words in [dst,dst+len) for heap-looking values.
    // Avoids GCTib mid-construct SEGV (slotwriter 1262c912).
    static void NoteStructWords(BaseObject* holder, MAddress dst, size_t dstLen);

    // Called when minor enqueue sees an invalid object from a known slot.
    static void OnInvalidEnqueue(BaseObject* object, BaseObject* holder, MAddress slot, MAddress raw,
                                 const char* origin);

    // T0: RecordCrossGenEdge decision for this slot (reason codes match RemsetPhaseProbe::SkipReason).
    // recorded=1 ⇒ entered remset; holderYoung/valueYoung are post-write region facts.
    static void NoteRemsetDecision(MAddress slot, BaseObject* holder, BaseObject* value, uint8_t reason,
                                   bool recorded, bool holderYoung, bool valueYoung);

    // T1 consume side: remset drain / live filter / rescan / ref_fix visits of a slot.
    // stage: "drain" | "live" | "rescan" | "reffix_remset" | "reffix_obj"
    static void NoteRemsetConsume(MAddress slot, const char* stage);

    // Optional: dump ring stats at process end / abort site.
    static void FlushSummary(const char* site);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_SLOT_WRITER_PROBE_H
