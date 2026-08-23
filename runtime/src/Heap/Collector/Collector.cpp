// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Collector/Collector.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Collector/GcStats.h"
#include "Common/BaseObject.h"
#include "Common/ColourPredicates.h"
#include "Common/StateWord.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/ManagedObjectGate.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {
const char* const COLLECTOR_NAME[] = { "No Collector", "Proxy Collector", "Regional-Copying Collector",
                                       "Smooth Collector" };

// zc7fix: is_mark_good fast path may admit plain non-heap slots (g_cjMarkBadMask all-zero on
// uncoloured non-null). Count rejects before IsValidObject/IsMarkedObject.
std::atomic<size_t> g_markGoodHeapGateReject{ 0 };

std::atomic<size_t> g_geomCrossEndReject{ 0 };
std::atomic<bool> g_geomAtexit{ false };

void EnsureGeomAtexit()
{
    bool expected = false;
    if (g_geomAtexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() {
            std::fprintf(stderr, "[GCV2][tailslot] geom_cross_end=%zu\n",
                         g_geomCrossEndReject.load(std::memory_order_relaxed));
            std::fflush(stderr);
        });
    }
}

// Smallest plausible TypeInfo / binary address without touching tip payload.
//
// markfloor caught RawArray+8 with small MArray::length (e.g. 0x200) via 64KiB.
// fys0segv: same interior shape with **large** length (observed tip=0x1fda868 /
// 0x2793ea8 under e75 ALOT FYS=0 → GetSize SEGV at tip+8, clear_satb young mark).
// Length is a size count; PIE TypeInfo / TIM mmap live well above 4GiB under ASLR.
// Raising the floor rejects large-length interiors so TryRecoverInteriorBase can
// re-host them — does NOT relax the gate (stricter reject set only).
constexpr uintptr_t kMinPlausibleTypeInfoAddr = 0x100000000ULL;

// fysfloor3: TypeInfo never lives at N*4GiB. TIM mmap(nullptr) 1MB arenas and
// PIE/static modules always have a non-zero page offset. Windows ImageBase
// 0x140000000 has low32=0x40000000 ≠ 0. Observed FYS=0 GetSize MAPERR family
// (compile+24GB N=20): tip=0x{3,5,6,7,8,9,b,d,19}00000000 = 15/20.
// Rejecting low32==0 is stricter-only — does not relax the gate.
inline bool TipLow32IsZero(uintptr_t tipAddr)
{
    return (tipAddr & 0xffffffffULL) == 0;
}

bool ObjectFitsInRegion(BaseObject* obj, RegionInfo* region)
{
    if (obj == nullptr || region == nullptr) {
        return false;
    }
    if (region->IsLargeRegion()) {
        return true;
    }
    MAddress objAddr = reinterpret_cast<MAddress>(obj);
    MAddress regionEnd = region->GetRegionEnd();
    if (objAddr >= regionEnd) {
        return false;
    }
    size_t objSize = obj->GetSize();
    if (objSize == 0 || (objSize % 8) != 0) {
        return false;
    }
    return objSize <= (regionEnd - objAddr);
}

// interiorsrc2: classify tip word without calling IsVaildType (may SEGV on bad tip).
bool TipWordLooksLikeTypeInfo(uintptr_t tipAddr)
{
    if (tipAddr == 0 || tipAddr < kMinPlausibleTypeInfoAddr) {
        return false;
    }
    if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return false;
    }
    if (TipLow32IsZero(tipAddr)) {
        return false;
    }
    if (Heap::IsHeapAddress(tipAddr)) {
        return false;
    }
    if (!TypeInfoManager::GetTypeInfoManager().IsResidentTypeInfoAddress(tipAddr)) {
        return false;
    }
    return true;
}

// If obj is interior into a managed object, return offset (8..64) else 0.
// Only peeks tip at obj-k; never walks payload. n7 GetSize crash was +40.
unsigned ClassifyInteriorOffset(BaseObject* obj)
{
    auto base = reinterpret_cast<uintptr_t>(obj);
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(base);
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion() ||
        region->GetRegionType() == RegionInfo::RegionType::FREE_REGION) {
        return 0;
    }
    unsigned offset = 0;
    for (unsigned k : { 8u, 16u, 24u, 32u, 40u, 48u, 56u, 64u }) {
        if (base < k) {
            continue;
        }
        auto* cand = reinterpret_cast<BaseObject*>(base - k);
        if (!Heap::IsHeapAddress(cand)) {
            continue;
        }
        RegionInfo* candidateRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(cand));
        if (candidateRegion == nullptr || candidateRegion != region || candidateRegion->IsFreeRegion() ||
            candidateRegion->IsGarbageRegion() ||
            candidateRegion->GetRegionType() == RegionInfo::RegionType::FREE_REGION) {
            continue;
        }
        // Safe: tip is first word; heap address already checked.
        uintptr_t tipAddr = reinterpret_cast<uintptr_t>(cand->GetTypeInfo());
        if (TipWordLooksLikeTypeInfo(tipAddr)) {
            if (offset != 0) {
                return 0;
            }
            offset = k;
        }
    }
    return offset;
}

// introot: host object for a heap interior (RawArray+8/...). nullptr if not interior.
// writeback2: knownBase from derived pairing wins over 8/16/24/32 tip scan.
BaseObject* RecoverInteriorBaseImpl(BaseObject* obj, BaseObject* knownBase)
{
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return nullptr;
    }
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(obj->GetTypeInfo());
    if (TipWordLooksLikeTypeInfo(tipAddr)) {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
        if (region != nullptr && ObjectFitsInRegion(obj, region)) {
            return nullptr;
        }
    }
    if (knownBase != nullptr && Heap::IsHeapAddress(knownBase) && knownBase != obj) {
        uintptr_t hostTip = reinterpret_cast<uintptr_t>(knownBase->GetTypeInfo());
        if (TipWordLooksLikeTypeInfo(hostTip)) {
            uintptr_t o = reinterpret_cast<uintptr_t>(obj);
            uintptr_t b = reinterpret_cast<uintptr_t>(knownBase);
            if (o > b && (o - b) <= 4096u) {
                return knownBase;
            }
        }
    }
    unsigned off = ClassifyInteriorOffset(obj);
    if (off == 0) {
        return nullptr;
    }
    return reinterpret_cast<BaseObject*>(reinterpret_cast<uintptr_t>(obj) - off);
}
} // namespace

void MaskEquivAtexitReport() {}

bool MaskEquivOn()
{
    return false;
}

bool MaskEquivInjectOn()
{
    return false;
}

void MaskEquivCheck(const EpochColours& e, const BadMasks& m)
{
    (void)e;
    (void)m;
}

bool Collector::MarkGoodHeapGate(const char* site, BaseObject* target)
{
    (void)site;
    if (Heap::IsHeapAddress(target)) {
        return true;
    }
    g_markGoodHeapGateReject.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void Collector::ReportMarkGoodHeapGateCounts() {}

bool PlausibleManagedObjectGate(const char* site, BaseObject* obj)
{
    if (obj == nullptr) {
        return false;
    }
    if (!Heap::IsHeapAddress(obj)) {
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion() ||
        region->GetRegionType() == RegionInfo::RegionType::FREE_REGION) {
        return false;
    }
    TypeInfo* tip = obj->GetTypeInfo();
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    if (tipAddr == 0 || tipAddr < kMinPlausibleTypeInfoAddr ||
        (tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0 || TipLow32IsZero(tipAddr) ||
        Heap::IsHeapAddress(tipAddr) ||
        !TypeInfoManager::GetTypeInfoManager().IsResidentTypeInfoAddress(tipAddr)) {
        return false;
    }
    if (!ObjectFitsInRegion(obj, region)) {
        EnsureGeomAtexit();
        size_t gn = g_geomCrossEndReject.fetch_add(1, std::memory_order_relaxed) + 1;
        if (gn <= 16 || (gn & 0x3ffU) == 0) {
            MAddress objAddr = reinterpret_cast<MAddress>(obj);
            MAddress rEnd = region->GetRegionEnd();
            LOG(RTLOG_ERROR,
                "[GCV2][tailslot] REJECT site=%s obj=%p tip=%p objSize=%zu regionEnd=%#zx remain=%zu n=%zu",
                site, obj, tip, obj->GetSize(), static_cast<size_t>(rEnd),
                objAddr < rEnd ? static_cast<size_t>(rEnd - objAddr) : 0, gn);
        }
        return false;
    }
    return true;
}

bool Collector::PlausibleManagedObjectGate(const char* site, BaseObject* obj)
{
    return MapleRuntime::PlausibleManagedObjectGate(site, obj);
}

BaseObject* Collector::TryRecoverInteriorBase(BaseObject* obj, BaseObject* knownBase)
{
    return RecoverInteriorBaseImpl(obj, knownBase);
}

void Collector::ReportPlausibleManagedObjectGateCounts()
{
    std::fprintf(stderr, "[GCV2][tailslot] geom_cross_end=%zu\n",
                 g_geomCrossEndReject.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

// F5: when FindToVersion returns null, never silently hand back a dead/zeroed from.
// Legal null (high-live / raw-pin survivor still at from, ghost=0) keeps returning obj.
// Illegal null (D: old tag + ghost already dispelled + from cleared) fails loudly here.
// See reports/REPORT-nullenum.md LEGAL_NULL_SET; reports/REPORT-tagaba.md F5.
// Anchor main 9ad991c4e8660c26d6bfe575f6425e1b227bdf94.
BaseObject* Collector::FindLatestVersion(BaseObject* obj) const
{
    if (obj == nullptr) {
        return nullptr;
    }

    BaseObject* to = FindToVersion(obj);
    if (to != nullptr) {
        if (to != obj && Heap::IsHeapAddress(to) && !to->IsValidObject()) {
            CHECK_DETAIL(obj->IsValidObject(),
                         "FindLatestVersion: route dest %p has no tip and from %p is not valid",
                         to, obj);
            return obj;
        }
        return to;
    }
    CHECK_DETAIL(obj->IsValidObject(),
                 "FindLatestVersion: no to-version for invalid from-object %p "
                 "(stale old-tag after ghost dispel; do not fall back to from)",
                 obj);
    return obj;
}

// The positional table this replaced still carried names from an older phase
// enum, so indices 12, 13 and 14 printed "forward phase", "enum fix phase" and
// "trace fix phase" for POST_TRACE, PREFORWARD and FORWARD. Every crash report
// naming a phase past CLEAR_SATB_BUFFER therefore named the wrong one, and a
// reader comparing two reports could not tell. Switching on the enum keeps the
// name attached to the value, so adding a phase is a compile error here rather
// than a silent relabelling of the phases after it.
const char* Collector::GetGCPhaseName(GCPhase phase)
{
    switch (phase) {
        case GC_PHASE_UNDEF: return "undefined phase";
        case GC_PHASE_IDLE: return "idle phase";
        case GC_PHASE_FINISH: return "finish phase";
        case GC_PHASE_RECLAIM_SATB_NODE: return "reclaim satb phase";
        case GC_PHASE_INIT: return "init phase";
        case GC_PHASE_ENUM: return "enum phase";
        case GC_PHASE_TRACE: return "trace phase";
        case GC_PHASE_CLEAR_SATB_BUFFER: return "clear satb phase";
        case GC_PHASE_POST_TRACE: return "post trace phase";
        case GC_PHASE_PREFORWARD: return "preforward phase";
        case GC_PHASE_FORWARD: return "forward phase";
    }
    return "unknown phase";
}

Collector::Collector() {}

const char* Collector::GetCollectorName() const { return COLLECTOR_NAME[collectorType]; }

void Collector::RequestGC(GCReason reason, bool async)
{
    RequestGCInternal(reason, async);
}

// Virtual default: this collector type does not implement the method. Always abort;
// body is out-of-line so Collector.h stays free of FormatLog / string payloads.
[[noreturn]] void Collector::AbortUnimplemented(const char* method)
{
    Logger::GetLogger().FormatLog(RTLOG_FATAL, true,
                                  "unimplemented virtual %s on this Collector "
                                  "(base default must not be reached)",
                                  method != nullptr ? method : "?");
    std::abort();
}
} // namespace MapleRuntime.
