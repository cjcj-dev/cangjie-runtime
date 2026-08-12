// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_VERIFY_ROOTS_H
#define MRT_VERIFY_ROOTS_H

#include <cstddef>
#include <cstdint>

#include "Common/BaseObject.h"
#include "Common/TypeDef.h"
#include "StackMap/StackMapTypeDef.h"

namespace MapleRuntime {

// Root kinds (invariant S applicability):
//   SLOT_STACK   — stackmap slot roots (AS1 boxes). Can verify S.
//   REG_ROOT     — register roots. Can verify S.
//   DERIVED_PTR  — interior pointers; cannot apply full S (not object head).
//   STATIC_ROOT  — static fields. Can verify S when heap target.
//   RUNTIME_ROOT — raw runtime roots. Can verify S when heap.
//   STACK_OBJECT — stack-address object via CheckAndPush. Can verify S.
//   STACK_PTR    — stack-grow tables; N/A for GC liveness S.
//
// Gate: MRT_GCV2_VERIFY_ROOTS=1 (default off). Diagnostics only — never replaces CHECK_DETAIL.

enum class RootKind : uint8_t {
    SLOT_STACK = 0,
    REG_ROOT,
    DERIVED_PTR,
    STATIC_ROOT,
    RUNTIME_ROOT,
    STACK_OBJECT,
    UNKNOWN,
};

enum class AddrRegion : uint8_t {
    NULL_ADDR = 0,
    HEAP,
    STACK,
    ZAP_PATTERN,
    LOW_NON_HEAP,
    OTHER,
};

struct RootVerifyContext {
    const char* phase = "unknown";
    RootKind kind = RootKind::UNKNOWN;
    const char* funcName = nullptr;
    uintptr_t startIP = 0;
    uintptr_t frameIP = 0;
    uintptr_t frameFA = 0;
    intptr_t slotBias = 0;
    int regNum = -1;
};

class VerifyRoots {
public:
    static bool Enabled();
    static const char* KindName(RootKind kind);
    static const char* RegionName(AddrRegion region);
    static AddrRegion ClassifyAddress(uintptr_t addr);
    static bool IsZapPattern(uintptr_t value);

    // Rich diagnostic for one root payload. Never aborts; never skips the real CHECK.
    static void VerifyRootPayload(const RootVerifyContext& ctx, void* slotOrRegAddr, BaseObject* obj);

    // Immediately before CheckAndPush IsVaildType CHECK_DETAIL (Mutator.cpp).
    static void BeforeCheckAndPush(BaseObject* obj);

    // Debug visitors for TracingCollector::VisitStackRoots (called before RootVisitor).
    static SlotDebugVisitor MakeSlotDebugVisitor(const RootVerifyContext& baseCtx);
    static RegDebugVisitor MakeRegDebugVisitor(const RootVerifyContext& baseCtx);

    static size_t BadRootCount();
    // INFO-channel count (typeinfo-misaligned demoted off BAD_ROOT; gcvheap2).
    static size_t InfoRootCount();
    static void ResetStats();
};

} // namespace MapleRuntime

#endif // MRT_VERIFY_ROOTS_H
