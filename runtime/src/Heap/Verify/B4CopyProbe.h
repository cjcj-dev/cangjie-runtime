// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_B4_COPY_PROBE_H
#define MRT_HEAP_B4_COPY_PROBE_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {

class BaseObject;

// Distinguishes whether CopyObject carries / introduces / is followed by
// fixup that produces offset-16 interior refs (B-4 b4copy).
// Gate (default off): MRT_GCV2_B4COPY=1
// Dump cap: MRT_GCV2_B4COPY_DUMP_MAX=<N> (default 64)
//
// Verdicts:
//   B4C_PREEXISTING     — src slot already interior; memmove preserves it
//   B4C_COPY_INTRODUCES — src base, dst interior after memmove
//   B4C_FIXUP_INTRODUCES— post-copy rewrite installs interior
//   B4C_LAYOUT_SHIFT    — byte-compare src/dst field words disagree (bad size/start)
class B4CopyProbe {
public:
    static bool Enabled();

    // Immediately before memmove_s in CopyCollector::CopyObject.
    static void NotePreCopy(const BaseObject& fromObj, size_t size);

    // Immediately after memmove_s (and optional TSan shadow fix).
    static void NotePostCopy(const BaseObject& fromObj, BaseObject& toObj, size_t size);

    // After a successful ref-field CAS in TryUpdateRefFieldImpl (fixup path).
    static void NoteFixupWrite(BaseObject* holder, void* slot, void* newVal, const char* site);

    static void FlushSummary(const char* site);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_B4_COPY_PROBE_H
