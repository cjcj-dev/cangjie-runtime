// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_ZAP_H
#define MRT_HEAP_ZAP_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {

// HotSpot-inspired poison patterns (see reports/REPORT-gcvroot.md ZAP_MECHANISM):
//   badHeapWordVal ≈ 0xBAADBABE (32-bit word; this tree lacks globalDefinitions.hpp)
//   badOopVal      ≈ 0x2BAD4B0BBAADBABE (64-bit)
// We use a 64-bit repeating pattern that cannot be a valid heap object head:
//   ZAP_WORD = 0xBAADF00DBAADF00D  ("bad food" × 2; low 3 bits = 101 ⇒ misaligned tip)
//
// Gates (default off):
//   MRT_GCV2_ZAP_RECLAIM=1  — fill reclaimed region payload (Z2)
//   MRT_GCV2_ZAP_ALLOC=1    — fill freshly allocated object bytes before ctor (Z3)
// Z1 (AS1 stack slots) needs cjcj codegen prologue fill → not done in runtime.

class HeapZap {
public:
    static constexpr uint64_t ZAP_WORD = 0xBAADF00DBAADF00DULL;
    static constexpr uint32_t ZAP_WORD32 = 0xBAADF00DU;

    static bool ReclaimEnabled();
    static bool AllocEnabled();
    static bool IsZapWord(uintptr_t value);

    // Fill [addr, addr+size) with ZAP_WORD. size need not be multiple of 8.
    static void Fill(void* addr, size_t size);

    // Z2: poison reclaimed region body [start, end).
    static void ZapReclaimedRegion(MAddress start, MAddress end);

    // Z3: poison newly allocated object storage before class header install.
    static void ZapAllocated(MAddress addr, size_t size);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_ZAP_H
