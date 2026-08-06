// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_B4_PERSIST_PROBE_H
#define MRT_HEAP_B4_PERSIST_PROBE_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {

// Timeline probe for remset-consumed offset=16 interiors (B-4 b4persist).
// Gate (default off): MRT_GCV2_B4PERSIST=1
// Cap: MRT_GCV2_B4PERSIST_CAP=<N> table slots (default 1<<20)
// Dump: MRT_GCV2_B4PERSIST_DUMP_MAX=<N> (default 64)
//
// Records last typed install / bulk range cover per field slot; on remset
// interior push, correlates write-time kind vs consume-time value.
class B4PersistProbe {
public:
    static bool Enabled();

    // Typed ref install (SetTargetObject / SetFieldValue / CAS).
    static void NoteTypedWrite(void* slot, void* value, const char* kind, void* ra0, void* ra1, void* ra2);

    // Bulk memmove/memcpy destination range (aligned field slots only).
    static void NoteBulkRange(MAddress start, size_t size, const char* site);

    // Remset path is about to PushYoungObject(target) for this field slot.
    // target is the resolved object pointer (may be interior).
    static void NoteRemsetConsume(MAddress slot, void* target);

    static void FlushSummary(const char* site);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_B4_PERSIST_PROBE_H
