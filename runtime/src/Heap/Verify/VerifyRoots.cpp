// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/VerifyRoots.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Base/LogFile.h"
#include "Common/StateWord.h"
#include "Heap/Heap.h"
#include "Heap/Verify/Zap.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/MClass.inline.h"

namespace MapleRuntime {
namespace {
std::atomic<size_t> g_badRootCount{ 0 };

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}
} // namespace

bool VerifyRoots::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_VERIFY_ROOTS");
    return on;
}

const char* VerifyRoots::KindName(RootKind kind)
{
    switch (kind) {
        case RootKind::SLOT_STACK:
            return "slot-stack";
        case RootKind::REG_ROOT:
            return "reg-root";
        case RootKind::DERIVED_PTR:
            return "derived-ptr";
        case RootKind::STATIC_ROOT:
            return "static-root";
        case RootKind::RUNTIME_ROOT:
            return "runtime-root";
        case RootKind::STACK_OBJECT:
            return "stack-object";
        default:
            return "unknown";
    }
}

const char* VerifyRoots::RegionName(AddrRegion region)
{
    switch (region) {
        case AddrRegion::NULL_ADDR:
            return "null";
        case AddrRegion::HEAP:
            return "heap";
        case AddrRegion::STACK:
            return "stack";
        case AddrRegion::ZAP_PATTERN:
            return "zap-pattern";
        case AddrRegion::LOW_NON_HEAP:
            return "low-non-heap";
        default:
            return "other";
    }
}

bool VerifyRoots::IsZapPattern(uintptr_t value)
{
    return HeapZap::IsZapWord(value);
}

AddrRegion VerifyRoots::ClassifyAddress(uintptr_t addr)
{
    if (addr == 0) {
        return AddrRegion::NULL_ADDR;
    }
    if (IsZapPattern(addr)) {
        return AddrRegion::ZAP_PATTERN;
    }
    if (Heap::IsHeapAddress(addr)) {
        return AddrRegion::HEAP;
    }
    if (addr < 0x10000ULL) {
        return AddrRegion::LOW_NON_HEAP;
    }
    Mutator* m = Mutator::GetMutator();
    if (m != nullptr && m->IsStackAddr(addr)) {
        return AddrRegion::STACK;
    }
    return AddrRegion::OTHER;
}

void VerifyRoots::VerifyRootPayload(const RootVerifyContext& ctx, void* slotOrRegAddr, BaseObject* obj)
{
    if (!Enabled()) {
        return;
    }
    if (obj == nullptr) {
        return; // invariant S: null is legal
    }

    uintptr_t objAddr = reinterpret_cast<uintptr_t>(obj);
    AddrRegion objRegion = ClassifyAddress(objAddr);

    bool bad = false;
    const char* reason = "ok";
    TypeInfo* tip = nullptr;
    uintptr_t tipAddr = 0;
    AddrRegion tipRegion = AddrRegion::NULL_ADDR;
    int typeByte = -1;

    if (objRegion == AddrRegion::ZAP_PATTERN) {
        bad = true;
        reason = "slot-value-is-zap-pattern";
    } else if (objRegion == AddrRegion::LOW_NON_HEAP) {
        bad = true;
        reason = "slot-value-low-non-heap";
    } else {
        tip = obj->GetTypeInfo();
        tipAddr = reinterpret_cast<uintptr_t>(tip);
        tipRegion = ClassifyAddress(tipAddr);
        if (tip == nullptr) {
            bad = true;
            reason = "null-typeinfo";
        } else if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
            bad = true;
            reason = "typeinfo-misaligned";
        } else if (tipRegion == AddrRegion::ZAP_PATTERN) {
            bad = true;
            reason = "typeinfo-is-zap-pattern";
        } else if (tipRegion == AddrRegion::HEAP) {
            // Known AS1 defect: tip residue lands in heap anonymous area, not TypeInfoManager.
            // Do not call tip->GetType() — tip is not a real TypeInfo; may SEGV/misread.
            bad = true;
            reason = "typeinfo-in-heap-not-typeinfo-manager";
            typeByte = static_cast<int>(*reinterpret_cast<const uint8_t*>(tipAddr));
        } else if (tipRegion == AddrRegion::LOW_NON_HEAP || tipRegion == AddrRegion::STACK ||
                   tipRegion == AddrRegion::ZAP_PATTERN) {
            bad = true;
            reason = "typeinfo-not-typeinfo-manager";
        } else if (!tip->IsVaildType()) {
            bad = true;
            reason = "invalid-type-kind";
            typeByte = static_cast<int>(tip->GetType());
        } else {
            typeByte = static_cast<int>(tip->GetType());
        }
    }

    if (!bad) {
        return;
    }

    g_badRootCount.fetch_add(1, std::memory_order_relaxed);
    const char* fname = (ctx.funcName != nullptr && ctx.funcName[0] != '\0') ? ctx.funcName : "?";
    VLOG(REPORT,
         "[GCV2][verify][roots] BAD_ROOT kind=%s phase=%s reason=%s "
         "func=%s startIP=%p frameIP=%p frameFA=%p pcOff=0x%zx "
         "slotBias=%zd reg=%d slotOrReg=%p "
         "obj=%p objRegion=%s tip=%p tipRegion=%s typeByte=%d "
         "env=MRT_GCV2_VERIFY_ROOTS=1",
         KindName(ctx.kind), ctx.phase, reason, fname,
         reinterpret_cast<void*>(ctx.startIP), reinterpret_cast<void*>(ctx.frameIP),
         reinterpret_cast<void*>(ctx.frameFA),
         (ctx.startIP != 0 && ctx.frameIP >= ctx.startIP) ? (ctx.frameIP - ctx.startIP) : 0,
         static_cast<ssize_t>(ctx.slotBias), ctx.regNum, slotOrRegAddr, obj, RegionName(objRegion),
         reinterpret_cast<void*>(tipAddr), RegionName(tipRegion), typeByte);
}

void VerifyRoots::BeforeCheckAndPush(BaseObject* obj)
{
    if (!Enabled() || obj == nullptr) {
        return;
    }
    RootVerifyContext ctx;
    ctx.phase = "CheckAndPush";
    ctx.kind = RootKind::STACK_OBJECT;
    VerifyRootPayload(ctx, nullptr, obj);
}

SlotDebugVisitor VerifyRoots::MakeSlotDebugVisitor(const RootVerifyContext& baseCtx)
{
    return [baseCtx](SlotBias bias, BaseObject* root) {
        RootVerifyContext ctx = baseCtx;
        ctx.kind = RootKind::SLOT_STACK;
        ctx.slotBias = static_cast<intptr_t>(bias);
        // slot address = FA + bias (SlotRoot: base + bias); FA is frameFA.
        void* slotAddr = nullptr;
        if (ctx.frameFA != 0) {
            slotAddr = reinterpret_cast<void*>(static_cast<intptr_t>(ctx.frameFA) + static_cast<intptr_t>(bias));
        }
        VerifyRootPayload(ctx, slotAddr, root);
    };
}

RegDebugVisitor VerifyRoots::MakeRegDebugVisitor(const RootVerifyContext& baseCtx)
{
    return [baseCtx](RegisterNum reg, const BaseObject* root) {
        RootVerifyContext ctx = baseCtx;
        ctx.kind = RootKind::REG_ROOT;
        ctx.regNum = static_cast<int>(reg);
        VerifyRootPayload(ctx, nullptr, const_cast<BaseObject*>(root));
    };
}

size_t VerifyRoots::BadRootCount()
{
    return g_badRootCount.load(std::memory_order_relaxed);
}

void VerifyRoots::ResetStats()
{
    g_badRootCount.store(0, std::memory_order_relaxed);
}

} // namespace MapleRuntime
