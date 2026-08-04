// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/WalkAlignProbe.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "Common/StateWord.h"
#include "Heap/Allocator/Allocator.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/TraceClear.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/MClass.inline.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {

std::atomic<size_t> g_totalSteps{ 0 };
std::atomic<size_t> g_badSteps{ 0 };
std::atomic<size_t> g_dumpsEmitted{ 0 };

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

size_t EnvSizeT(const char* name, size_t def)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return def;
    }
    char* end = nullptr;
    unsigned long n = std::strtoul(v, &end, 10);
    if (end == v) {
        return def;
    }
    return static_cast<size_t>(n);
}

const char* RegionTypeName(RegionInfo::RegionType t)
{
    switch (t) {
        case RegionInfo::RegionType::FREE_REGION:
            return "FREE";
        case RegionInfo::RegionType::THREAD_LOCAL_REGION:
            return "THREAD_LOCAL";
        case RegionInfo::RegionType::RECENT_FULL_REGION:
            return "RECENT_FULL";
        case RegionInfo::RegionType::FROM_REGION:
            return "FROM";
        case RegionInfo::RegionType::LONE_FROM_REGION:
            return "LONE_FROM";
        case RegionInfo::RegionType::UNMOVABLE_FROM_REGION:
            return "UNMOVABLE_FROM";
        case RegionInfo::RegionType::TO_REGION:
            return "TO";
        case RegionInfo::RegionType::FULL_PINNED_REGION:
            return "FULL_PINNED";
        case RegionInfo::RegionType::RECENT_PINNED_REGION:
            return "RECENT_PINNED";
        case RegionInfo::RegionType::RAW_POINTER_PINNED_REGION:
            return "RAW_POINTER_PINNED";
        case RegionInfo::RegionType::TL_RAW_POINTER_REGION:
            return "TL_RAW_POINTER";
        case RegionInfo::RegionType::TL_LARGE_RAW_POINTER_REGION:
            return "TL_LARGE_RAW_POINTER";
        case RegionInfo::RegionType::LARGE_REGION:
            return "LARGE";
        case RegionInfo::RegionType::RECENT_LARGE_REGION:
            return "RECENT_LARGE";
        case RegionInfo::RegionType::GARBAGE_REGION:
            return "GARBAGE";
        default:
            return "UNKNOWN";
    }
}

// Probe-side size that never calls through a null/invalid TypeInfo.
// Mirrors BaseObject::GetSize + RegionSpace::ToAllocSize shape; returns 0 on refuse.
size_t ProbeAllocSize(BaseObject* obj, const char*& refuseReason)
{
    refuseReason = "ok";
    if (obj == nullptr) {
        refuseReason = "null-obj";
        return 0;
    }
    TypeInfo* tip = obj->GetTypeInfo();
    if (tip == nullptr) {
        refuseReason = "null-typeinfo";
        return 0;
    }
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        refuseReason = "typeinfo-misaligned";
        return 0;
    }
    if (Heap::IsHeapAddress(tipAddr)) {
        refuseReason = "typeinfo-in-heap";
        return 0;
    }
    if (!tip->IsVaildType()) {
        refuseReason = "invalid-type-kind";
        return 0;
    }

    size_t objSize = 0;
    if (tip->IsArrayType()) {
        const MArray* mArray = reinterpret_cast<const MArray*>(obj);
        objSize = mArray->GetMArraySize();
    } else {
        objSize = static_cast<size_t>(tip->GetInstanceSize()) + TYPEINFO_PTR_SIZE;
    }
    if (objSize == 0) {
        refuseReason = "zero-obj-size";
        return 0;
    }
    return RegionSpace::ToAllocSize(objSize);
}

void DumpWords(uintptr_t addr, size_t nWords)
{
    for (size_t i = 0; i < nWords; ++i) {
        uintptr_t a = addr + i * sizeof(uint64_t);
        uint64_t w = 0;
        // Best-effort raw read; region is heap, expected mapped.
        w = *reinterpret_cast<uint64_t*>(a);
        VLOG(REPORT, "[GCV2][walkalign] word[%zu]@%#zx = %#llx", i, a, static_cast<unsigned long long>(w));
    }
}

void DumpFirstBad(RegionInfo* region, uintptr_t position, uintptr_t allocPtr, size_t stepInRegion, BaseObject* prev,
                  size_t prevSize, const char* reason, size_t probeSize)
{
    size_t maxDumps = EnvSizeT("MRT_GCV2_WALK_ALIGN_MAX_DUMPS", 1);
    size_t n = g_dumpsEmitted.fetch_add(1, std::memory_order_acq_rel);
    if (n >= maxDumps) {
        return;
    }

    GCPhase phase = GCPhase::GC_PHASE_UNDEF;
    const char* phaseName = "undef";
    // Heap is live whenever VisitAllObjects runs; phase read is best-effort.
    phase = Heap::GetHeap().GetGCPhase();
    phaseName = Collector::GetGCPhaseName(phase);

    uintptr_t regionStart = region != nullptr ? region->GetRegionStart() : 0;
    RegionInfo::RegionType rtype =
        region != nullptr ? region->GetRegionType() : RegionInfo::RegionType::FREE_REGION;
    bool young = region != nullptr && region->IsYoungRegion();

    VLOG(REPORT,
         "[GCV2][walkalign] FIRST_BAD_STEP reason=%s dump=%zu totalSteps=%zu "
         "regionStart=%#zx regionType=%s young=%u regionAllocPtr=%#zx "
         "cursor=%#zx stepInRegion=%zu probeSize=%zu "
         "gcPhase=%u(%s) env=MRT_GCV2_WALK_ALIGN=1",
         reason, n + 1, g_totalSteps.load(std::memory_order_relaxed), regionStart, RegionTypeName(rtype),
         young ? 1u : 0u, allocPtr, position, stepInRegion, probeSize, static_cast<unsigned>(phase), phaseName);

    if (prev != nullptr) {
        TypeInfo* pti = prev->GetTypeInfo();
        const char* pName = "?";
        size_t pInst = 0;
        bool pArr = false;
        const char* prevRefuse = "ok";
        size_t recompute = ProbeAllocSize(prev, prevRefuse);
        if (pti != nullptr && !Heap::IsHeapAddress(reinterpret_cast<uintptr_t>(pti))) {
            pName = pti->GetName() != nullptr ? pti->GetName() : "?";
            pInst = static_cast<size_t>(pti->GetInstanceSize());
            pArr = pti->IsArrayType();
        }
        uintptr_t prevAddr = reinterpret_cast<uintptr_t>(prev);
        size_t gapToCursor = position > prevAddr ? position - prevAddr : 0;
        VLOG(REPORT,
             "[GCV2][walkalign] PREDECESSOR addr=%#zx ti=%s isArray=%u instSize=%zu "
             "prevRecordedSize=%zu recomputeSize=%zu recomputeRefuse=%s gapToCursor=%zu "
             "sizeMatchesGap=%u",
             prevAddr, pName, pArr ? 1u : 0u, pInst, prevSize, recompute, prevRefuse, gapToCursor,
             (prevSize == gapToCursor) ? 1u : 0u);
        DumpWords(prevAddr, 4);
    } else {
        VLOG(REPORT, "[GCV2][walkalign] PREDECESSOR none (first object in region or large)");
    }

    VLOG(REPORT, "[GCV2][walkalign] BAD_CURSOR raw words (3+header):");
    DumpWords(position, 4);

    // Distance hint: scan forward a few alloc-align steps for a plausible next TypeInfo.
    size_t hintGap = 0;
    for (size_t d = Allocator::ALLOC_ALIGN; d <= 256 && position + d < allocPtr; d += Allocator::ALLOC_ALIGN) {
        BaseObject* cand = reinterpret_cast<BaseObject*>(position + d);
        TypeInfo* cti = cand->GetTypeInfo();
        if (cti == nullptr) {
            continue;
        }
        uintptr_t cAddr = reinterpret_cast<uintptr_t>(cti);
        if ((cAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
            continue;
        }
        if (Heap::IsHeapAddress(cAddr)) {
            continue;
        }
        if (!cti->IsVaildType()) {
            continue;
        }
        hintGap = d;
        const char* cName = cti->GetName() != nullptr ? cti->GetName() : "?";
        VLOG(REPORT,
             "[GCV2][walkalign] HINT_NEXT_PLAUSIBLE_HEAD offset=+%zu addr=%#zx ti=%s "
             "(gdbheavy R2 shape: cursor may be true-start - 8B)",
             d, position + d, cName);
        break;
    }
    if (hintGap == 0) {
        VLOG(REPORT, "[GCV2][walkalign] HINT_NEXT_PLAUSIBLE_HEAD none in +256B");
    }

    // E1(b): if TRACE_CLEAR is on, report whether the hole falls inside a zeroed range.
    if (TraceClear::Enabled()) {
        char clearBuf[256];
        bool inClear = TraceClear::Lookup(static_cast<MAddress>(position), clearBuf, sizeof(clearBuf));
        VLOG(REPORT, "[GCV2][walkalign] TRACE_CLEAR_LOOKUP inClear=%u detail=%s", inClear ? 1u : 0u, clearBuf);
    }
}

} // namespace

bool WalkAlignProbe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_WALK_ALIGN");
    return on;
}

bool WalkAlignProbe::CheckBeforeSize(RegionInfo* region, uintptr_t position, uintptr_t allocPtr, size_t stepInRegion,
                                     BaseObject* prev, size_t prevSize, size_t& totalStepsOut)
{
    size_t steps = g_totalSteps.fetch_add(1, std::memory_order_relaxed) + 1;
    totalStepsOut = steps;
    // Heartbeat once so "probe armed but no FIRST_BAD" is distinguishable from "probe never ran".
    if (steps == 1) {
        VLOG(REPORT, "[GCV2][walkalign] ARMED first_step regionStart=%#zx cursor=%#zx env=MRT_GCV2_WALK_ALIGN=1",
             region != nullptr ? region->GetRegionStart() : 0, position);
    }

    if ((position & (Allocator::ALLOC_ALIGN - 1)) != 0) {
        g_badSteps.fetch_add(1, std::memory_order_relaxed);
        DumpFirstBad(region, position, allocPtr, stepInRegion, prev, prevSize, "cursor-misaligned", 0);
        return false;
    }
    if (position >= allocPtr) {
        g_badSteps.fetch_add(1, std::memory_order_relaxed);
        DumpFirstBad(region, position, allocPtr, stepInRegion, prev, prevSize, "cursor-past-allocPtr", 0);
        return false;
    }

    BaseObject* obj = reinterpret_cast<BaseObject*>(position);
    const char* refuse = "ok";
    size_t size = ProbeAllocSize(obj, refuse);
    if (size == 0) {
        g_badSteps.fetch_add(1, std::memory_order_relaxed);
        DumpFirstBad(region, position, allocPtr, stepInRegion, prev, prevSize, refuse, 0);
        return false;
    }
    if (position + size > allocPtr) {
        g_badSteps.fetch_add(1, std::memory_order_relaxed);
        DumpFirstBad(region, position, allocPtr, stepInRegion, prev, prevSize, "size-overruns-region", size);
        return false;
    }
    if ((size & (Allocator::ALLOC_ALIGN - 1)) != 0) {
        g_badSteps.fetch_add(1, std::memory_order_relaxed);
        DumpFirstBad(region, position, allocPtr, stepInRegion, prev, prevSize, "size-not-aligned", size);
        return false;
    }
    return true;
}

size_t WalkAlignProbe::TotalSteps() { return g_totalSteps.load(std::memory_order_relaxed); }

size_t WalkAlignProbe::BadSteps() { return g_badSteps.load(std::memory_order_relaxed); }

} // namespace MapleRuntime
