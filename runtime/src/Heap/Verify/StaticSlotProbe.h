// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_STATIC_SLOT_PROBE_H
#define MRT_HEAP_STATIC_SLOT_PROBE_H

#include <cstddef>
#include <cstdint>

#include "ObjectModel/RefField.h"

namespace MapleRuntime {

// Read-only probe: on each static-root slot visit, classify the slot value
// (object-base vs interior) and resolve the *slot address* to maps/module/symbol.
// Gate (default off): MRT_GCV2_STATIC_SLOT=1
// Optional dump cap:   MRT_GCV2_STATIC_SLOT_DUMP_MAX=<N> (default 96)
class StaticSlotProbe {
public:
    static bool Enabled();

    // Called from VisitMinorRootSlots while gMinorRootOrigin=="static".
    // field is the static root RefField (slot storage); &field is the slot address.
    static void NoteStaticField(RefField<>& field);

    static void FlushSummary(const char* site);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_STATIC_SLOT_PROBE_H
