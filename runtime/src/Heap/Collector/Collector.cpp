// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Collector/Collector.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

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
std::atomic<size_t> g_markGoodHeapGateSample{ 0 };

// markfloor: interiors pass IsHeapAddress + IsValidObject (tip word non-null) but tip is
// not a TypeInfo*. 0x200 observed = MArray::length at RawArray+8.
std::atomic<size_t> g_plausibleObjGateReject{ 0 };
std::atomic<size_t> g_plausibleObjGateSample{ 0 };
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
// interiorsrc2: per-site reject counters (always on when gate accounts).
std::atomic<size_t> g_plausibleObjGateBySite[16]{ {} };
// ⭐⭐⭐ 0809 00:5x：**按 interior offset 计数，⛔ 不经采样**。
//   ⛔ 为什么必须加：⭐ `int_off` 原来只在**被采样的那几行日志**里出现
//     ⇒ ⭐⭐ 而采样预算是**按 GC 轮**分的、⭐ 且被所有 reason 共享
//     ⇒ ⭐⭐⭐ 于是 `int_off=8 REJECT=0` 有**两个**含义：⭐ 真的没有 · ⭐ 预算被别的 reason 用光了
//   ⭐ 实账：⭐ `c1remeasure` 读到 **0**、⭐ `getsize7` 同判据读到 **12830** ⇒ ⭐ 两棒都没错，
//     ⭐⭐ **是判据本身建在采样输出上** —— ⭐ 而 C1 的完成判据正是它。
//   ⇒ ⭐ 只在诊断模式（`MRT_GCV2_MARKFLOOR_OBJ_GATE=1`）下计，⭐ 产品路径零代价。
//   索引：0=非内点 · 1=off8 · 2=off16 · 3=off24 · 4=off32
std::atomic<size_t> g_plausibleObjGateByIntOff[5]{ {} };

// gatebase: exact accounting for the mark-and-remember reject slow path.  The
// existing gate census is process-wide and groups this site into "other"; it
// cannot answer whether a single minor rejected recoverable interiors or bad
// payload words.  This census is still behind MRT_GCV2_MARKFLOOR_OBJ_GATE=1,
// so the default write-barrier path pays nothing.
enum GateBaseRejectReason : unsigned {
    GATEBASE_NON_HEAP,
    GATEBASE_DEAD_REGION,
    GATEBASE_NULL_TIP,
    GATEBASE_SMALL_INT,
    GATEBASE_MISALIGNED,
    GATEBASE_4G_ALIGNED,
    GATEBASE_TIP_IN_HEAP,
    GATEBASE_NOT_RESIDENT,
    GATEBASE_CROSS_END,
    GATEBASE_REASON_COUNT,
};
std::atomic<size_t> g_gateBaseRejectByReason[GATEBASE_REASON_COUNT]{ {} };
std::atomic<size_t> g_gateBaseInteriorByOffset[9]{ {} }; // 0=not interior; 1..8 => +8..+64

// The sample budget is per GC cycle, not per process.
//
// It used to be a plain global that only ever counted up, so the gate printed at most 48
// lines for the entire run. On a workload that rejects thousands of roots that means the
// side which explains *why* a root was dropped goes silent within the first cycle, while
// the side which shows the *consequence* ([GCRECLAIM][fwd-empty-collect]) keeps printing
// uncapped -- 50,282 lines in 60s was measured. nwdiag (0808) had to mark the
// REJECT-obj <-> CollectRegion-region reconciliation NOT_RUN for exactly this reason: by
// the time the interesting regions were collected the gate had stopped talking.
std::atomic<size_t> g_plausibleObjGateSampleGc{ std::numeric_limits<size_t>::max() };

bool PlausibleObjGateSampleAllowed(size_t budget)
{
    size_t gc = g_gcCount.load(std::memory_order_relaxed);
    if (g_plausibleObjGateSampleGc.load(std::memory_order_relaxed) != gc) {
        g_plausibleObjGateSampleGc.store(gc, std::memory_order_relaxed);
        g_plausibleObjGateSample.store(0, std::memory_order_relaxed);
    }
    return g_plausibleObjGateSample.fetch_add(1, std::memory_order_relaxed) < budget;
}

bool MarkGoodHeapGateAccountOn()
{
    static const bool on = []() {
        const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_MARKGOOD_HEAP_GATE */;
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

bool PlausibleObjGateAccountOn()
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_MARKFLOOR_OBJ_GATE");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
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

// gatehot GATEEQUIV: dual-run the pure reject/admit decision under a second independent
// implementation and count mismatches. Default off. Positive control:
//   MRT_GCV2_GATEEQUIV_INJECT=1 forces one synthetic mismatch so a silent harness is visible.
// Report via g_gateEquivMismatch / g_gateEquivChecked (read under MRT_GCV2_GATEEQUIV=1).
std::atomic<size_t> g_gateEquivMismatch{ 0 };
std::atomic<size_t> g_gateEquivChecked{ 0 };
std::atomic<size_t> g_gateEquivInjected{ 0 };

void GateEquivAtexitReport()
{
    LOG(RTLOG_ERROR,
        "[GCV2][gateequiv] checked=%zu mismatch=%zu inject=%zu env=MRT_GCV2_GATEEQUIV=1",
        g_gateEquivChecked.load(std::memory_order_relaxed),
        g_gateEquivMismatch.load(std::memory_order_relaxed),
        g_gateEquivInjected.load(std::memory_order_relaxed));
}

bool GateEquivOn()
{
    static const bool on = []() {
        const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_GATEEQUIV */;
        bool enabled = v != nullptr && std::strcmp(v, "1") == 0;
        if (enabled) {
            std::atexit(GateEquivAtexitReport);
        }
        return enabled;
    }();
    return on;
}

bool GateEquivInjectOn()
{
    static const bool on = []() {
        const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_GATEEQUIV_INJECT */;
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

// c4unify MASKEQUIV counters (see Collector.h for the contract).
std::atomic<size_t> g_maskEquivChecked{ 0 };
std::atomic<size_t> g_maskEquivMismatch{ 0 };
std::atomic<size_t> g_maskEquivInjected{ 0 };

bool ObjectFitsInRegion(BaseObject* obj, RegionInfo* region);

// Pure predicate copy: same reject set as PlausibleManagedObjectGate, no counters.
// Used only for GATEEQUIV dual-run. Must track the product gate branch-for-branch.
bool PlausibleManagedObjectGatePure(BaseObject* obj)
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
    if (tipAddr == 0) {
        return false;
    }
    if (tipAddr < kMinPlausibleTypeInfoAddr) {
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
    return ObjectFitsInRegion(obj, region);
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
                if (PlausibleObjGateAccountOn()) {
                    static std::atomic<size_t> g_recoverKnown{ 0 };
                    size_t s = g_recoverKnown.fetch_add(1, std::memory_order_relaxed);
                    if (s < 16) {
                        LOG(RTLOG_ERROR,
                            "[GCV2][introot-recover] interior=%p off=%zu host=%p hostTip=%p src=derived-base",
                            obj, static_cast<size_t>(o - b), knownBase, knownBase->GetTypeInfo());
                    }
                }
                return knownBase;
            }
        }
    }
    unsigned off = ClassifyInteriorOffset(obj);
    if (off == 0) {
        return nullptr;
    }
    auto* base = reinterpret_cast<BaseObject*>(reinterpret_cast<uintptr_t>(obj) - off);
    if (PlausibleObjGateAccountOn()) {
        static std::atomic<size_t> g_recoverSample{ 0 };
        size_t s = g_recoverSample.fetch_add(1, std::memory_order_relaxed);
        if (s < 16) {
            LOG(RTLOG_ERROR,
                "[GCV2][introot-recover] interior=%p off=%u host=%p hostTip=%p src=heuristic",
                obj, off, base, base->GetTypeInfo());
        }
    }
    return base;
}

void NoteGateBaseReject(const char* site, BaseObject* obj, GateBaseRejectReason reason)
{
    if (site == nullptr || std::strcmp(site, "mark_and_remember") != 0) {
        return;
    }
    g_gateBaseRejectByReason[reason].fetch_add(1, std::memory_order_relaxed);

    BaseObject* base = RecoverInteriorBaseImpl(obj, nullptr);
    if (base == nullptr || base == obj || !Heap::IsHeapAddress(base)) {
        g_gateBaseInteriorByOffset[0].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    RegionInfo* objectRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    RegionInfo* baseRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(base));
    uintptr_t objectAddress = reinterpret_cast<uintptr_t>(obj);
    uintptr_t baseAddress = reinterpret_cast<uintptr_t>(base);
    if (objectRegion == nullptr || objectRegion != baseRegion || baseAddress >= objectAddress ||
        !TipWordLooksLikeTypeInfo(reinterpret_cast<uintptr_t>(base->GetTypeInfo())) ||
        !TypeInfoManager::GetTypeInfoManager().ContainsTypeInfo(base->GetTypeInfo())) {
        g_gateBaseInteriorByOffset[0].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    size_t offset = objectAddress - baseAddress;
    size_t allocSize = RegionSpace::GetAllocSize(*base);
    if (offset == 0 || offset >= allocSize || offset > 64 || (offset % 8) != 0) {
        g_gateBaseInteriorByOffset[0].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_gateBaseInteriorByOffset[offset / 8].fetch_add(1, std::memory_order_relaxed);
}

unsigned SiteBucket(const char* site)
{
    if (site == nullptr) {
        return 15;
    }
    if (std::strstr(site, "MarkObject") != nullptr) {
        return 0;
    }
    if (std::strstr(site, "TraceRefField") != nullptr) {
        return 1;
    }
    if (std::strstr(site, "EnumRefField") != nullptr) {
        return 2;
    }
    if (std::strstr(site, "EnumAndTagRawRoot") != nullptr) {
        return 3;
    }
    if (std::strstr(site, "ForwardUpdateRawRef") != nullptr) {
        return 4;
    }
    if (std::strstr(site, "ForwardObjectExclusive") != nullptr) {
        return 7;
    }
    if (std::strstr(site, "TryForward") != nullptr) {
        return 6;
    }
    if (std::strstr(site, "ForwardObject") != nullptr) {
        return 5;
    }
    if (std::strstr(site, "TraceYoungClosure") != nullptr) {
        return 8;
    }
    if (std::strstr(site, "PushYoungObject") != nullptr ||
        std::strstr(site, "AdmitYoungObject") != nullptr) {
        return 9;
    }
    // getsize7: dense region walks that call GetSize/GetAllocSize without a prior gate.
    if (std::strstr(site, "VisitLiveObjects") != nullptr) {
        return 10;
    }
    if (std::strstr(site, "VisitAllObjects") != nullptr) {
        return 11;
    }
    if (std::strstr(site, "CompactRegion") != nullptr) {
        return 12;
    }
    // getsizetrace: pinned free-slot list GetSize (PopFront / PushFront).
    if (std::strstr(site, "SlotList") != nullptr ||
        std::strstr(site, "FreePinnedSlotLists") != nullptr) {
        return 13;
    }
    return 14;
}
} // namespace

void MaskEquivAtexitReport()
{
    LOG(RTLOG_ERROR, "[GCV2][maskequiv] checked=%zu mismatch=%zu inject=%zu env=MRT_GCV2_MASKEQUIV=1",
        g_maskEquivChecked.load(std::memory_order_relaxed), g_maskEquivMismatch.load(std::memory_order_relaxed),
        g_maskEquivInjected.load(std::memory_order_relaxed));
}

bool MaskEquivOn()
{
    static const bool on = []() {
        const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_MASKEQUIV */;
        bool enabled = v != nullptr && std::strcmp(v, "1") == 0;
        if (enabled) {
            std::atexit(MaskEquivAtexitReport);
        }
        return enabled;
    }();
    return on;
}

bool MaskEquivInjectOn()
{
    static const bool on = []() {
        const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_MASKEQUIV_INJECT */;
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

// The witness below is a TEXT COPY of WCollector::set_good_masks as it stood at 6adf9dd0.
// ⛔ It must never become a call to ComputeBadMasks: that would compare one implementation with
// itself, report mismatch=0 for ever, and look exactly like success. This is the same trap the
// GATEEQUIV inject arm exists to expose.
void MaskEquivCheck(const EpochColours& e, const BadMasks& m)
{
    if (!MaskEquivOn()) {
        return;
    }
    uintptr_t wRemap = e.remappedYoungMask & e.remappedOldMask;
    uintptr_t wLoad = TAGGED_BITS_MASK | (REMAP_COLOUR_MASK ^ wRemap);
    uintptr_t wMark = wLoad | (MARKED_YOUNG_MASK & ~e.markedYoung) | (MARKED_OLD_MASK & ~e.markedOld);
    uintptr_t wStore = wMark | (REMEMBERED_MASK & ~e.remembered);
    uintptr_t wStoreGood = wRemap | e.markedYoung | e.markedOld | e.remembered;

    bool injected = false;
    if (MaskEquivInjectOn() && g_maskEquivInjected.fetch_add(1, std::memory_order_relaxed) == 0) {
        // Synthetic divergence, exactly once: flips a bit no colour family owns, so the arm
        // proves the comparison runs without teaching the comparison anything real.
        wLoad ^= (uintptr_t(1) << 63);
        injected = true;
    }

    g_maskEquivChecked.fetch_add(1, std::memory_order_relaxed);
    const bool mismatch =
        (wRemap != m.remapColour) || (wLoad != m.loadBad) || (wMark != m.markBad) || (wStore != m.storeBad) ||
        (wStoreGood != m.storeGood) || ((wStoreGood ^ STORE_METADATA_MASK) != m.storeBad);
    if (mismatch) {
        size_t n = g_maskEquivMismatch.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 16) {
            LOG(RTLOG_ERROR,
                "[GCV2][maskequiv] %s n=%zu remap=%#zx/%#zx load=%#zx/%#zx mark=%#zx/%#zx store=%#zx/%#zx "
                "(product/witness)",
                injected ? "INJECT mismatch" : "mismatch", n, static_cast<size_t>(m.remapColour),
                static_cast<size_t>(wRemap), static_cast<size_t>(m.loadBad), static_cast<size_t>(wLoad),
                static_cast<size_t>(m.markBad), static_cast<size_t>(wMark), static_cast<size_t>(m.storeBad),
                static_cast<size_t>(wStore));
        }
    }
}

// goodpred: only reached when MRT_GCV2_ZGC_LOADGOOD or MRT_GCV2_LOADGOOD_AUDIT is set;
// Collector::is_load_good keeps the legacy expression inline for the default mode.
bool Collector::is_load_good_switched(RefField<>& ref, uint8_t site) const
{
    if (UNLIKELY(GoodPredDiag::SelfTestPending())) {
        const uintptr_t mask = static_cast<uintptr_t>(::g_cjLoadBadMask);
        const uintptr_t colour = ColourPredicates::current_remapped(mask);
        // A payload the address bits accept; nothing dereferences it, both predicates are
        // pure bit tests. Mid-evacuation is no longer a pointer bit; stale remap is load-bad.
        const uintptr_t payload = 0x0000700000001000ULL;
        const uintptr_t taggedValue = payload | (REMAP_COLOUR_MASK ^ colour);
        const uintptr_t plainValue = payload | colour;
        RefField<> taggedProbe(to_zpointer(static_cast<MAddress>(taggedValue)));
        RefField<> plainProbe(to_zpointer(static_cast<MAddress>(plainValue)));
        GoodPredDiag::ReportSelfTest(
            taggedValue,
            !is_null(taggedProbe.GetTargetObject()) && is_young_load_good(taggedProbe) &&
                is_old_load_good(taggedProbe),
            ColourPredicates::is_load_good(taggedValue, mask), plainValue,
            !is_null(plainProbe.GetTargetObject()) && is_young_load_good(plainProbe) &&
                is_old_load_good(plainProbe),
            ColourPredicates::is_load_good(plainValue, mask));
    }
    const uintptr_t value = static_cast<uintptr_t>(raw(ref.GetFieldValue()));
    const bool zgc = ColourPredicates::is_load_good(value, static_cast<uintptr_t>(::g_cjLoadBadMask));
    if (GoodPredDiag::g_mode == GoodPredDiag::kZgc) {
        return zgc;
    }
    const bool legacy = !is_null(ref.GetTargetObject()) && is_young_load_good(ref) && is_old_load_good(ref);
    return GoodPredDiag::NoteAudit(value, legacy, zgc, site);
}

bool Collector::MarkGoodHeapGate(const char* site, BaseObject* target)
{
    if (Heap::IsHeapAddress(target)) {
        return true;
    }
    size_t n = g_markGoodHeapGateReject.fetch_add(1, std::memory_order_relaxed) + 1;
    // Always count; samples use RTLOG_ERROR so they survive default log level and abrupt exit.
    if (MarkGoodHeapGateAccountOn()) {
        size_t s = g_markGoodHeapGateSample.fetch_add(1, std::memory_order_relaxed);
        if (s < 8) {
            LOG(RTLOG_ERROR, "[GCV2][markgood-heap-gate] REJECT site=%s target=%p n=%zu", site, target, n);
        }
    }
    return false;
}

void Collector::ReportMarkGoodHeapGateCounts()
{
    if (!MarkGoodHeapGateAccountOn()) {
        return;
    }
    LOG(RTLOG_ERROR, "[GCV2][markgood-heap-gate] reject=%zu env=MRT_GCV2_MARKGOOD_HEAP_GATE=1",
        g_markGoodHeapGateReject.load(std::memory_order_relaxed));
}

// gatehot: product path no longer pays a shared-line atomic on every reject.
// Accounting (total / bysite / byintoff / samples) is entirely behind
// MRT_GCV2_MARKFLOOR_OBJ_GATE=1 — same switch that already gated SiteBucket.
// "Did the gate fire?" under product defaults → set that env and read
// ReportPlausibleManagedObjectGateCounts / [markfloor-obj-gate] reject= lines.
// Reject/admit predicate is bit-identical (GATEEQUIV); only side-effect counters move.
bool PlausibleManagedObjectGate(const char* site, BaseObject* obj)
{
    if (obj == nullptr) {
        if (GateEquivOn()) {
            g_gateEquivChecked.fetch_add(1, std::memory_order_relaxed);
            // pure also rejects null — match.
            if (GateEquivInjectOn() &&
                g_gateEquivInjected.fetch_add(1, std::memory_order_relaxed) == 0) {
                g_gateEquivMismatch.fetch_add(1, std::memory_order_relaxed);
                LOG(RTLOG_ERROR,
                    "[GCV2][gateequiv] INJECT mismatch site=%s obj=%p product=0 pure=1", site, obj);
            }
        }
        return false;
    }
    // gchot: SiteBucket is strstr over 13 tags — only needed for bysite accounting.
    // Product path (MARKFLOOR_OBJ_GATE unset) must not pay strstr on every reject.
    // Reject/admit predicate below is unchanged; GATEEQUIV = identical reject set.
    const bool account = PlausibleObjGateAccountOn();
    bool product = true;
    if (!Heap::IsHeapAddress(obj)) {
        product = false;
        if (account) {
            NoteGateBaseReject(site, obj, GATEBASE_NON_HEAP);
            size_t n = g_plausibleObjGateReject.fetch_add(1, std::memory_order_relaxed) + 1;
            g_plausibleObjGateBySite[SiteBucket(site)].fetch_add(1, std::memory_order_relaxed);
            if (PlausibleObjGateSampleAllowed(32)) {
                GCPhase phase = Heap::GetHeap().GetGCPhase();
                LOG(RTLOG_ERROR,
                    "[GCV2][markfloor-obj-gate] REJECT gc=%zu site=%s obj=%p reason=non-heap n=%zu phase=%s(%u) "
                    "ra0=%p ra1=%p ra2=%p",
                    g_gcCount.load(std::memory_order_relaxed), site, obj, n, Collector::GetGCPhaseName(phase),
                    static_cast<unsigned>(phase),
                    __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2));
            }
        }
    } else {
        // sizeguard: work stack may hold a pointer into a region whose payload still looks
        // like a valid object (stale/interior tip ⇒ plausible GetSize, but obj+size crosses
        // regionEnd). Reject by geometry, not region type: FREE/GARBAGE and full active
        // (regionType=2/3, allocPtr==end, +0xffe0/+0xfff0) share the shape.
        // ZGC: zMarkStackEntry.hpp:81 encodes object_address in bits 63-5 (no interior);
        // zPage.inline.hpp:188 is_in requires offset < top().
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
        if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion() ||
            region->GetRegionType() == RegionInfo::RegionType::FREE_REGION) {
            product = false;
            if (account) {
                NoteGateBaseReject(site, obj, GATEBASE_DEAD_REGION);
                size_t n = g_plausibleObjGateReject.fetch_add(1, std::memory_order_relaxed) + 1;
                g_plausibleObjGateBySite[SiteBucket(site)].fetch_add(1, std::memory_order_relaxed);
                if (PlausibleObjGateSampleAllowed(48)) {
                    GCPhase phase = Heap::GetHeap().GetGCPhase();
                    unsigned rtype = region == nullptr ? 255U : static_cast<unsigned>(region->GetRegionType());
                    uintptr_t rstart = region == nullptr ? 0 : region->GetRegionStart();
                    LOG(RTLOG_ERROR,
                        "[GCV2][markfloor-obj-gate] REJECT gc=%zu site=%s obj=%p reason=dead-region n=%zu "
                        "region=%p start=%#zx regionType=%u phase=%s(%u) ra0=%p ra1=%p ra2=%p",
                        g_gcCount.load(std::memory_order_relaxed), site, obj, n, region, rstart, rtype,
                        Collector::GetGCPhaseName(phase),
                        static_cast<unsigned>(phase), __builtin_return_address(0),
                        __builtin_return_address(1), __builtin_return_address(2));
                }
            }
        } else {
            // Read tip word only (StateWord load). Do not call IsVaildType / GetSize yet.
            TypeInfo* tip = obj->GetTypeInfo();
            uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
            const char* reason = nullptr;
            if (tipAddr == 0) {
                reason = "null-tip";
            } else if (tipAddr < kMinPlausibleTypeInfoAddr) {
                reason = "tip-small-int";
            } else if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
                reason = "tip-misaligned";
            } else if (TipLow32IsZero(tipAddr)) {
                reason = "tip-4g-aligned";
            } else if (Heap::IsHeapAddress(tipAddr)) {
                // TypeInfo lives in binary / TypeInfoManager mmap, never in managed heap.
                // Heap tip ⇒ interior into another object (classic B-4 shape).
                reason = "tip-in-heap";
            } else if (!TypeInfoManager::GetTypeInfoManager().IsResidentTypeInfoAddress(tipAddr)) {
                // ASCII / leftover payload can look like a 48-bit pointer
                // (aligned, ≥4GiB, low32≠0, not in heap). Residence is the
                // positive contract that rejects those without relaxing the gate.
                reason = "tip-not-resident";
            } else if (!ObjectFitsInRegion(obj, region)) {
                reason = "geom-cross-end";
            }
            if (reason != nullptr) {
                product = false;
                if (reason[0] == 'g' && std::strcmp(reason, "geom-cross-end") == 0) {
                    EnsureGeomAtexit();
                    size_t gn = g_geomCrossEndReject.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (gn <= 16 || (gn & 0x3ffU) == 0) {
                        MAddress objAddr = reinterpret_cast<MAddress>(obj);
                        MAddress rEnd = region == nullptr ? 0 : region->GetRegionEnd();
                        LOG(RTLOG_ERROR,
                            "[GCV2][tailslot] REJECT site=%s obj=%p tip=%p objSize=%zu regionEnd=%#zx remain=%zu n=%zu",
                            site, obj, tip, obj->GetSize(), static_cast<size_t>(rEnd),
                            objAddr < rEnd ? static_cast<size_t>(rEnd - objAddr) : 0, gn);
                    }
                }
                if (account) {
                    GateBaseRejectReason gateBaseReason = GATEBASE_NOT_RESIDENT;
                    if (std::strcmp(reason, "null-tip") == 0) {
                        gateBaseReason = GATEBASE_NULL_TIP;
                    } else if (std::strcmp(reason, "tip-small-int") == 0) {
                        gateBaseReason = GATEBASE_SMALL_INT;
                    } else if (std::strcmp(reason, "tip-misaligned") == 0) {
                        gateBaseReason = GATEBASE_MISALIGNED;
                    } else if (std::strcmp(reason, "tip-4g-aligned") == 0) {
                        gateBaseReason = GATEBASE_4G_ALIGNED;
                    } else if (std::strcmp(reason, "tip-in-heap") == 0) {
                        gateBaseReason = GATEBASE_TIP_IN_HEAP;
                    } else if (std::strcmp(reason, "geom-cross-end") == 0) {
                        gateBaseReason = GATEBASE_CROSS_END;
                    }
                    NoteGateBaseReject(site, obj, gateBaseReason);
                    size_t n = g_plausibleObjGateReject.fetch_add(1, std::memory_order_relaxed) + 1;
                    g_plausibleObjGateBySite[SiteBucket(site)].fetch_add(1, std::memory_order_relaxed);
                    // ⭐ 先无条件（诊断模式下）记 offset，⛔ 再谈采样 —— ⭐ 判据不能建在采样输出上
                    unsigned off = ClassifyInteriorOffset(obj);
                    g_plausibleObjGateByIntOff[off / 8u < 5u ? off / 8u : 0u].fetch_add(1, std::memory_order_relaxed);
                    if (PlausibleObjGateSampleAllowed(48)) {
                        GCPhase phase = Heap::GetHeap().GetGCPhase();
                        // The region this obj sits in is the join key against [GCRECLAIM][fwd-empty-collect]:
                        // if the dropped root's region is the one CollectRegion later frees, the "live array
                        // reclaimed as empty" chain is reconciled rather than inferred. Only the dead-region
                        // branch above used to print it, and this branch is the one RawArray+8 takes.
                        uintptr_t rstart = region == nullptr ? 0 : region->GetRegionStart();
                        // tip-small-int + off=8 ⇒ classic RawArray+8 / &MArray::length.
                        LOG(RTLOG_ERROR,
                            "[GCV2][markfloor-obj-gate] REJECT gc=%zu site=%s obj=%p tip=%p reason=%s n=%zu "
                            "int_off=%u region=%p start=%#zx phase=%s(%u) ra0=%p ra1=%p ra2=%p",
                            g_gcCount.load(std::memory_order_relaxed), site, obj, tip, reason, n, off, region, rstart,
                            Collector::GetGCPhaseName(phase), static_cast<unsigned>(phase),
                            __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2));
                    }
                }
            }
        }
    }

    if (GateEquivOn()) {
        bool pure = PlausibleManagedObjectGatePure(obj);
        g_gateEquivChecked.fetch_add(1, std::memory_order_relaxed);
        bool mismatch = (product != pure);
        if (GateEquivInjectOn() &&
            g_gateEquivInjected.fetch_add(1, std::memory_order_relaxed) == 0) {
            mismatch = true;
            LOG(RTLOG_ERROR,
                "[GCV2][gateequiv] INJECT mismatch site=%s obj=%p product=%d pure=%d",
                site, obj, static_cast<int>(product), static_cast<int>(pure));
        }
        if (mismatch) {
            size_t m = g_gateEquivMismatch.fetch_add(1, std::memory_order_relaxed) + 1;
            if (m <= 16) {
                LOG(RTLOG_ERROR,
                    "[GCV2][gateequiv] MISMATCH site=%s obj=%p product=%d pure=%d n=%zu",
                    site, obj, static_cast<int>(product), static_cast<int>(pure), m);
            }
        }
    }
    return product;
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
    if (GateEquivOn()) {
        LOG(RTLOG_ERROR,
            "[GCV2][gateequiv] checked=%zu mismatch=%zu inject=%zu env=MRT_GCV2_GATEEQUIV=1",
            g_gateEquivChecked.load(std::memory_order_relaxed),
            g_gateEquivMismatch.load(std::memory_order_relaxed),
            g_gateEquivInjected.load(std::memory_order_relaxed));
    }
    if (!PlausibleObjGateAccountOn()) {
        return;
    }
    size_t gateBaseReason[GATEBASE_REASON_COUNT] = {};
    size_t gateBaseOffset[9] = {};
    size_t gateBaseTotal = 0;
    size_t gateBaseInterior = 0;
    for (unsigned i = 0; i < GATEBASE_REASON_COUNT; ++i) {
        gateBaseReason[i] = g_gateBaseRejectByReason[i].exchange(0, std::memory_order_relaxed);
        gateBaseTotal += gateBaseReason[i];
    }
    for (unsigned i = 0; i < 9; ++i) {
        gateBaseOffset[i] = g_gateBaseInteriorByOffset[i].exchange(0, std::memory_order_relaxed);
        if (i != 0) {
            gateBaseInterior += gateBaseOffset[i];
        }
    }
    LOG(RTLOG_ERROR,
        "[GCV2][gatebase] reject=%zu interior=%zu bad=%zu "
        "reason=[nonheap:%zu,dead:%zu,null:%zu,small:%zu,misalign:%zu,4g:%zu,inheap:%zu,resident:%zu,cross:%zu] "
        "offset=[none:%zu,8:%zu,16:%zu,24:%zu,32:%zu,40:%zu,48:%zu,56:%zu,64:%zu]",
        gateBaseTotal, gateBaseInterior, gateBaseTotal - gateBaseInterior,
        gateBaseReason[GATEBASE_NON_HEAP], gateBaseReason[GATEBASE_DEAD_REGION],
        gateBaseReason[GATEBASE_NULL_TIP], gateBaseReason[GATEBASE_SMALL_INT],
        gateBaseReason[GATEBASE_MISALIGNED], gateBaseReason[GATEBASE_4G_ALIGNED],
        gateBaseReason[GATEBASE_TIP_IN_HEAP], gateBaseReason[GATEBASE_NOT_RESIDENT],
        gateBaseReason[GATEBASE_CROSS_END], gateBaseOffset[0], gateBaseOffset[1], gateBaseOffset[2],
        gateBaseOffset[3], gateBaseOffset[4], gateBaseOffset[5], gateBaseOffset[6], gateBaseOffset[7],
        gateBaseOffset[8]);
    LOG(RTLOG_ERROR, "[GCV2][markfloor-obj-gate] reject=%zu geom_cross_end=%zu env=MRT_GCV2_MARKFLOOR_OBJ_GATE=1",
        g_plausibleObjGateReject.load(std::memory_order_relaxed),
        g_geomCrossEndReject.load(std::memory_order_relaxed));
    // ⭐⭐ 这一行才是 C1 判据该读的：⭐ 未经采样的按 offset 全量计数
    LOG(RTLOG_ERROR,
        "[GCV2][markfloor-obj-gate] byintoff none=%zu off8=%zu off16=%zu off24=%zu off32=%zu",
        g_plausibleObjGateByIntOff[0].load(std::memory_order_relaxed),
        g_plausibleObjGateByIntOff[1].load(std::memory_order_relaxed),
        g_plausibleObjGateByIntOff[2].load(std::memory_order_relaxed),
        g_plausibleObjGateByIntOff[3].load(std::memory_order_relaxed),
        g_plausibleObjGateByIntOff[4].load(std::memory_order_relaxed));
    LOG(RTLOG_ERROR,
        "[GCV2][markfloor-obj-gate] bysite MarkObject=%zu TraceRefField=%zu EnumRefField=%zu "
        "EnumAndTagRawRoot=%zu ForwardUpdateRawRef=%zu ForwardObject=%zu TryForward=%zu "
        "ForwardObjectExclusive=%zu TraceYoungClosure=%zu PushYoungObject=%zu "
        "VisitLiveObjects=%zu VisitAllObjects=%zu CompactRegion=%zu SlotList=%zu other=%zu",
        g_plausibleObjGateBySite[0].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[1].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[2].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[3].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[4].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[5].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[6].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[7].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[8].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[9].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[10].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[11].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[12].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[13].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[14].load(std::memory_order_relaxed));
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

// Positive-control inject for assertbody Phase 2 (MRT_ASSERTBODY_PROBE=1|2|3).
// Fires on first RequestGC so SignalManager is already installed and rec=crash
// can capture assert=. 1=Collector named abort, 2=TracingCollector named abort,
// 3=FormatLog FATAL (CHECK-family). Off unless env set.
static void MaybeAssertbodyProbe()
{
    static std::atomic<bool> done{ false };
    if (done.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    const char* p = static_cast<const char*>(nullptr) /* pinned-off:MRT_ASSERTBODY_PROBE */;
    if (p == nullptr || p[0] == '\0' || std::strcmp(p, "0") == 0) {
        return;
    }
    if (std::strcmp(p, "1") == 0) {
        Collector::AbortUnimplemented("Collector::GetGCStats");
    }
    if (std::strcmp(p, "2") == 0) {
        Collector::AbortUnimplemented("TracingCollector::TraceObjectRefFields");
    }
    if (std::strcmp(p, "3") == 0) {
        Logger::GetLogger().FormatLog(RTLOG_FATAL, true, "Check failed: assertbody_probe_formatlog");
        std::abort();
    }
}

Collector::Collector() {}

const char* Collector::GetCollectorName() const { return COLLECTOR_NAME[collectorType]; }

void Collector::RequestGC(GCReason reason, bool async)
{
    MaybeAssertbodyProbe();
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
