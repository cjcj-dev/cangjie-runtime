// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/VerifyHeap.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN64)
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__) || defined(__OHOS__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Common/StateWord.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "Heap/Verify/YyEdgeDiag.h"
#include "ObjectModel/MClass.inline.h"
#include "ObjectModel/RefField.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {
constexpr size_t kSampleLimit = 8;
constexpr size_t kDefaultMaxFailures = 20;

// Same floor as Collector.cpp:97 TipWordLooksLikeTypeInfo / RegionInfo.h:1211.
// TypeInfo lives in PIE / TIM mmap, well above 4GiB. A small integer here is
// payload / interior / leftover, not a TypeInfo. IsVaildType / GetType read
// type@+8 (TYPE_KIND_MAX=0x18). Observed POST_EVAC crash: tip=0x10 →
// si_addr=0x18 SEGV_MAPERR (zstripe baseline_serial_post, si_code=1).
constexpr uintptr_t kMinPlausibleTypeInfoAddr = 0x100000000ULL;

bool TipAddrLooksPlausible(uintptr_t tipAddr)
{
    if (tipAddr < kMinPlausibleTypeInfoAddr) {
        return false;
    }
    if ((tipAddr & 0xffffffffULL) == 0) {
        return false;
    }
    return true;
}

// Verifier-only: is the TypeInfo page mapped? Product paths never load type@+8
// on leftover payload tips. After the 4GiB floor, POST_EVAC still SEGV'd on
// unmapped tips (cand-fork rbx=0x6c6275700a70 → si_addr=+8; basic cand
// rbx=0x3500020428). mincore/VirtualQuery report that; they do not relax
// IsVaildType / IsValidObject / PlausibleManagedObjectGate.
bool TipPageIsMapped(uintptr_t tipAddr)
{
    static thread_local uintptr_t lastMappedPage = 0;
#if defined(_WIN64)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const uintptr_t pageSize = static_cast<uintptr_t>(info.dwPageSize);
#else
    static const uintptr_t pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
#endif
    if (pageSize == 0) {
        return false;
    }
    const uintptr_t page = tipAddr & ~(pageSize - 1);
    if (page == lastMappedPage) {
        return true;
    }
#if defined(_WIN64)
    MEMORY_BASIC_INFORMATION memoryInfo;
    if (VirtualQuery(reinterpret_cast<const void*>(tipAddr), &memoryInfo, sizeof(memoryInfo)) == 0) {
        return false;
    }
    if (memoryInfo.State != MEM_COMMIT || (memoryInfo.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }
    lastMappedPage = page;
    return true;
#elif defined(__linux__) || defined(__APPLE__) || defined(__OHOS__)
    // mincore's out-parameter is spelled differently per platform: Darwin and
    // the BSDs declare it `char*`, Linux and OHOS declare it `unsigned char*`,
    // and the two are distinct types. The previous form cast `&vec` to
    // `unsigned char*` at the call, which pinned the argument to one platform's
    // spelling no matter what the platform actually declares. Name the type once
    // here and pass `&vec` uncast, so this alias is the only place the spelling
    // is decided.
#if defined(__APPLE__)
    using MincoreVec = char;
#else
    using MincoreVec = unsigned char;
#endif
    MincoreVec vec = 0;
    if (mincore(reinterpret_cast<void*>(page), pageSize, &vec) != 0) {
        (void)errno;
        return false;
    }
    lastMappedPage = page;
    return true;
#else
    return false;
#endif
}

bool TipAddrSafeToDereference(uintptr_t tipAddr)
{
    return TipAddrLooksPlausible(tipAddr) && TipPageIsMapped(tipAddr);
}

// Defect channel (BAD_OBJ): real invariant-H breaks — invalid-kind / tip-in-heap /
// null tip / invalid object / bad region / bad ref. INFO channel: typeinfo-misaligned
// is a true phenomenon but not defect D (gcvtag CORE_PC); keep counting, never filter it out.
enum class HeapVerifyChannel : uint8_t { Ok = 0, Defect, Info };

struct HeapVerifyStats {
    size_t objectsScanned = 0;
    size_t h1InvalidObject = 0;
    size_t h2NullTip = 0;
    size_t h2MisalignedTip = 0; // INFO channel only
    size_t h2TipInHeap = 0;
    size_t h2InvalidTypeKind = 0;
    size_t h2TipInTim = 0; // tip in TypeInfoManager mmap (positive online hit)
    size_t h2TipNonHeapOk = 0;
    size_t h3BadRef = 0;
    size_t h3ReachableHolder = 0;
    size_t h3UnreachableHolder = 0;
    size_t h3ReachabilityUnknown = 0;
    size_t h4BadRegion = 0;
    size_t failures = 0;     // defect-channel count (drives FATAL / maxFailures)
    size_t infoCount = 0;    // INFO-channel count (misaligned etc.)
    size_t truncated = 0;
    size_t infoTruncated = 0;
    uint64_t costNs = 0;
    std::array<void*, kSampleLimit> samples{};
    size_t sampleCount = 0;
};

bool EnvEnabled(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

size_t EnvSizeT(const char* name, size_t defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return defaultValue;
    }
    char* end = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) {
        return defaultValue;
    }
    return static_cast<size_t>(parsed);
}

void PushSample(HeapVerifyStats& stats, void* addr)
{
    if (stats.sampleCount < kSampleLimit) {
        stats.samples[stats.sampleCount++] = addr;
    }
}

// H2 online TypeInfo region test:
//   1) null → DEFECT
//   2) misaligned → INFO (true phenomenon; not defect D — gcvtag CORE_PC)
//   3) tip ∈ heap address range → DEFECT (defect D: tip in heap anonymous)
//   4) tip below the product-path floor (Collector.cpp TipWordLooksLikeTypeInfo)
//      or on an unmapped page → DEFECT without dereference. Old code called
//      IsVaildType here and SEGV'd (postevac: tip=0x10 → si_addr=0x18;
//      leftover payload tip=0x6c6275700a70 → si_addr=+8).
//   5) !IsVaildType() → DEFECT (type byte ≥ TYPE_KIND_MAX)
//   6) TypeInfoManager::ContainsAddress(tip) → strongest online positive
//   7) else non-heap + valid type → accept (static TypeInfo in load module)
// Anchor intent: HotSpot oop verify + our gcvroot tipRegion==HEAP reject.
// Channel split: VERIFY_HEAP reason reclass (gcvheap2) — misaligned no longer floods BAD_OBJ.
HeapVerifyChannel CheckTypeInfoRegion(TypeInfo* tip, HeapVerifyStats& stats, const char*& reason)
{
    if (tip == nullptr) {
        ++stats.h2NullTip;
        reason = "null-typeinfo";
        return HeapVerifyChannel::Defect;
    }
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        ++stats.h2MisalignedTip;
        reason = "typeinfo-misaligned";
        return HeapVerifyChannel::Info;
    }
    if (Heap::IsHeapAddress(tipAddr)) {
        ++stats.h2TipInHeap;
        reason = "typeinfo-in-heap";
        return HeapVerifyChannel::Defect;
    }
    // TIM mmap is mapped by construction — accept before the page probe.
    if (TypeInfoManager::GetTypeInfoManager().ContainsAddress(tipAddr)) {
        if (!tip->IsVaildType()) {
            ++stats.h2InvalidTypeKind;
            reason = "invalid-type-kind";
            return HeapVerifyChannel::Defect;
        }
        ++stats.h2TipInTim;
        reason = "ok";
        return HeapVerifyChannel::Ok;
    }
    // Product PlausibleManagedObjectGate rejects this set before any TypeInfo
    // field load. The verifier must report it, not crash on type@+8.
    if (!TipAddrSafeToDereference(tipAddr)) {
        ++stats.h2InvalidTypeKind;
        reason = "typeinfo-implausible-addr";
        return HeapVerifyChannel::Defect;
    }
    if (!tip->IsVaildType()) {
        ++stats.h2InvalidTypeKind;
        reason = "invalid-type-kind";
        return HeapVerifyChannel::Defect;
    }
    ++stats.h2TipNonHeapOk;
    reason = "ok";
    return HeapVerifyChannel::Ok;
}

HeapVerifyChannel CheckObjectH1H2(BaseObject* obj, HeapVerifyStats& stats, const char*& reason)
{
    if (obj == nullptr) {
        reason = "null-object";
        return HeapVerifyChannel::Defect;
    }
    if (!obj->IsValidObject()) {
        ++stats.h1InvalidObject;
        reason = "invalid-object";
        return HeapVerifyChannel::Defect;
    }
    return CheckTypeInfoRegion(obj->GetTypeInfo(), stats, reason);
}

const char* RegionKindName(RegionInfo* region)
{
    if (region == nullptr) {
        return "null-region";
    }
    if (region->IsFreeRegion()) {
        return "free";
    }
    if (region->IsGarbageRegion()) {
        return "garbage";
    }
    if (region->IsYoungRegion()) {
        return "young";
    }
    if (region->IsFromRegion()) {
        return "from";
    }
    if (region->IsUnmovableFromRegion()) {
        return "unmovable-from";
    }
    if (region->IsLargeRegion()) {
        return "large";
    }
    return "old-or-other";
}

void ReportDefect(HeapVerifyStats& stats, size_t maxFailures, const char* reason, BaseObject* obj,
                  BaseObject* related, int typeByte)
{
    ++stats.failures;
    if (stats.failures > maxFailures) {
        ++stats.truncated;
        return;
    }
    PushSample(stats, obj);
    RegionInfo* region = obj == nullptr ? nullptr : RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    uintptr_t header0 = 0;
    uintptr_t header1 = 0;
    TypeInfo* tip = nullptr;
    if (obj != nullptr) {
        // Object header is StateWord (typeinfo packing) then optional next word.
        const uintptr_t* raw = reinterpret_cast<const uintptr_t*>(obj);
        header0 = raw[0];
        header1 = raw[1];
        tip = obj->GetTypeInfo();
    }
    VLOG(REPORT,
         "[GCV2][verify][heap] BAD_OBJ reason=%s obj=%p related=%p typeByte=%d "
         "region=%s regionBase=%p tip=%p header0=%#zx header1=%#zx "
         "failure=%zu max=%zu env=MRT_GCV2_VERIFY_HEAP=1",
         reason, obj, related, typeByte, RegionKindName(region),
         region == nullptr ? nullptr : reinterpret_cast<void*>(region->GetRegionStart()),
         tip, static_cast<size_t>(header0), static_cast<size_t>(header1), stats.failures, maxFailures);
}

void ReportInfo(HeapVerifyStats& stats, size_t maxFailures, const char* reason, BaseObject* obj,
                BaseObject* related, int typeByte)
{
    ++stats.infoCount;
    if (stats.infoCount > maxFailures) {
        ++stats.infoTruncated;
        return;
    }
    VLOG(REPORT,
         "[GCV2][verify][heap] INFO reason=%s obj=%p related=%p typeByte=%d "
         "info=%zu max=%zu env=MRT_GCV2_VERIFY_HEAP=1",
         reason, obj, related, typeByte, stats.infoCount, maxFailures);
}

void ReportByChannel(HeapVerifyChannel ch, HeapVerifyStats& stats, size_t maxFailures, const char* reason,
                     BaseObject* obj, BaseObject* related, int typeByte)
{
    if (ch == HeapVerifyChannel::Defect) {
        ReportDefect(stats, maxFailures, reason, obj, related, typeByte);
    } else if (ch == HeapVerifyChannel::Info) {
        ReportInfo(stats, maxFailures, reason, obj, related, typeByte);
    }
}

int SampleTypeByte(BaseObject* obj)
{
    if (obj == nullptr || !obj->IsValidObject()) {
        return -1;
    }
    TypeInfo* tip = obj->GetTypeInfo();
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    // Only sample typeByte when tip is a mapped TypeInfo. Misaligned / small-int
    // / 4GiB-aligned tips are not TypeInfo (typeByte=-128 was a garbage load;
    // tip=0x10 SEGV'd at type@+8 — see REPORT-gcvtag / postevac).
    if (tip != nullptr && (tipAddr & StateWord::ADDRESS_ALIGN_MASK) == 0 &&
        TipAddrSafeToDereference(tipAddr) && !Heap::IsHeapAddress(tipAddr)) {
        return static_cast<int>(tip->GetType());
    }
    return -1;
}

void ReportH3BadRegion(HeapVerifyStats& stats, size_t maxFailures, const char* point, BaseObject* target,
                       BaseObject* holder, RefField<>& field,
                       const std::unordered_set<BaseObject*>* rootReachableHolders)
{
    const bool reachabilityKnown = rootReachableHolders != nullptr;
    const bool holderReachable = reachabilityKnown && rootReachableHolders->count(holder) != 0;
    if (!reachabilityKnown) {
        ++stats.h3ReachabilityUnknown;
    } else if (holderReachable) {
        ++stats.h3ReachableHolder;
    } else {
        ++stats.h3UnreachableHolder;
    }

    ++stats.failures;
    // Reachable holders are the product-path question.  Do not let the
    // default 20-sample cap hide them behind earlier dead inventory.
    constexpr size_t kReachablePrintCap = 64;
    if (holderReachable) {
        if (stats.h3ReachableHolder > kReachablePrintCap) {
            return;
        }
    } else if (stats.failures > maxFailures) {
        ++stats.truncated;
        return;
    }
    PushSample(stats, target);

    RegionInfo* targetRegion =
        target == nullptr ? nullptr : RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    RegionInfo* holderRegion =
        holder == nullptr ? nullptr : RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
    uintptr_t targetHeader0 = 0;
    uintptr_t targetHeader1 = 0;
    TypeInfo* targetTip = nullptr;
    if (target != nullptr) {
        const uintptr_t* raw = reinterpret_cast<const uintptr_t*>(target);
        targetHeader0 = raw[0];
        targetHeader1 = raw[1];
        targetTip = target->GetTypeInfo();
    }

    TypeInfo* holderTip = holder == nullptr ? nullptr : holder->GetTypeInfo();
    const char* holderType = holderTip == nullptr || holderTip->GetName() == nullptr ? "?" : holderTip->GetName();
    const unsigned inThisVec = YyEdgeDiag::Enabled() && YyEdgeDiag::HolderInThisProductVec(holder) ? 1u : 0u;
    const unsigned inPrevVec = YyEdgeDiag::Enabled() && YyEdgeDiag::HolderInPrevProductVec(holder) ? 1u : 0u;

    VLOG(REPORT,
         "[GCV2][verify][heap] BAD_OBJ reason=h3-target-bad-region point=%s "
         "obj=%p related=%p field=%p fieldOffset=%zd typeByte=%d "
         "region=%s regionBase=%p tip=%p header0=%#zx header1=%#zx "
         "holderRegion=%s holderRegionBase=%p holderTip=%p holderType=%s "
         "holderReachabilityKnown=%u holderReachable=%u "
         "inThisProductVec=%u inPrevProductVec=%u "
         "failure=%zu max=%zu env=MRT_GCV2_VERIFY_HEAP=1",
         point == nullptr ? "?" : point, target, holder, &field, BaseObject::FieldOffset(holder, &field),
         SampleTypeByte(target), RegionKindName(targetRegion),
         targetRegion == nullptr ? nullptr : reinterpret_cast<void*>(targetRegion->GetRegionStart()), targetTip,
         static_cast<size_t>(targetHeader0), static_cast<size_t>(targetHeader1), RegionKindName(holderRegion),
         holderRegion == nullptr ? nullptr : reinterpret_cast<void*>(holderRegion->GetRegionStart()), holderTip,
         holderType, static_cast<unsigned>(reachabilityKnown), static_cast<unsigned>(holderReachable),
         inThisVec, inPrevVec, stats.failures, maxFailures);
}
} // namespace

void VerifyHeapObjects(const char* point, bool force, const std::unordered_set<BaseObject*>* rootReachableHolders)
{
    // Default off — HotSpot VerifyBeforeGC/VerifyAfterGC DIAGNOSTIC pattern.
    // force=true lets post-evac run without enabling the global pre-evacuate gate.
    if (!force && !EnvEnabled("MRT_GCV2_VERIFY_HEAP")) {
        return;
    }

    static std::atomic<size_t> invokeCount{ 0 };
    size_t invoke = invokeCount.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t startAt = EnvSizeT("MRT_GCV2_VERIFY_HEAP_START_AT", 0);
    if (startAt != 0 && invoke < startAt) {
        return;
    }
    size_t every = EnvSizeT("MRT_GCV2_VERIFY_HEAP_EVERY", 1);
    if (every == 0) {
        every = 1;
    }
    if (startAt != 0) {
        if ((invoke - startAt) % every != 0) {
            return;
        }
    } else if ((invoke - 1) % every != 0) {
        return;
    }

    size_t maxFailures = EnvSizeT("MRT_GCV2_VERIFY_HEAP_MAX_FAILURES", kDefaultMaxFailures);
    if (maxFailures == 0) {
        maxFailures = kDefaultMaxFailures;
    }

    uint64_t startNs = TimeUtil::NanoSeconds();
    HeapVerifyStats stats;

    Heap::GetHeap().ForEachObj(
        [&stats, maxFailures, point, rootReachableHolders](BaseObject* obj) {
            if (obj == nullptr) {
                return;
            }
            ++stats.objectsScanned;
            const char* reason = "ok";
            int typeByte = -1;

            // H4: region state must admit a live object.
            RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
            if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
                ++stats.h4BadRegion;
                ReportDefect(stats, maxFailures, "bad-region", obj, nullptr, typeByte);
                return;
            }

            // H1 + H2 on holder (defect vs INFO channel split).
            HeapVerifyChannel ch = CheckObjectH1H2(obj, stats, reason);
            if (ch != HeapVerifyChannel::Ok) {
                typeByte = SampleTypeByte(obj);
                ReportByChannel(ch, stats, maxFailures, reason, obj, nullptr, typeByte);
                return;
            }

            // H3: each ref field null or target satisfies H1+H2+H4.
            // H4 on the target is the leave-alone / reclaimed-from case: header bits
            // can still look valid (H1+H2 pass) while the unit is already free/garbage.
            if (!obj->HasRefField()) {
                return;
            }
            obj->ForEachRefField([&stats, maxFailures, point, rootReachableHolders, obj](RefField<>& field) {
                BaseObject* target = to_object(field.GetTargetObject());
                if (target == nullptr) {
                    return;
                }
                if (!Heap::IsHeapAddress(target)) {
                    // Non-heap target (e.g. static / foreign) — not invariant H scope.
                    return;
                }
                RegionInfo* tRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                if (tRegion == nullptr || tRegion->IsFreeRegion() || tRegion->IsGarbageRegion()) {
                    ++stats.h3BadRef;
                    ++stats.h4BadRegion;
                    ReportH3BadRegion(stats, maxFailures, point, target, obj, field, rootReachableHolders);
                    return;
                }
                const char* tReason = "ok";
                HeapVerifyChannel tCh = CheckObjectH1H2(target, stats, tReason);
                if (tCh != HeapVerifyChannel::Ok) {
                    ++stats.h3BadRef;
                    int tByte = SampleTypeByte(target);
                    ReportByChannel(tCh, stats, maxFailures, tReason, target, obj, tByte);
                }
            });
        },
        false);

    stats.costNs = TimeUtil::NanoSeconds() - startNs;

    VLOG(REPORT,
         "[GCV2][verify][heap] point=%s invoke=%zu env=MRT_GCV2_VERIFY_HEAP=1 "
         "objects=%zu failures=%zu info=%zu truncated=%zu infoTruncated=%zu "
         "H1_invalid=%zu H2_nullTip=%zu H2_misalign=%zu H2_tipInHeap=%zu H2_badKind=%zu "
         "H2_tipInTim=%zu H2_tipNonHeapOk=%zu H3_badRef=%zu "
         "H3_reachableHolder=%zu H3_unreachableHolder=%zu H3_reachabilityUnknown=%zu H4_badRegion=%zu "
         "costNs=%llu maxFailures=%zu "
         "samples=[%p,%p,%p,%p]",
         point == nullptr ? "?" : point, invoke, stats.objectsScanned, stats.failures, stats.infoCount,
         stats.truncated, stats.infoTruncated, stats.h1InvalidObject, stats.h2NullTip, stats.h2MisalignedTip,
         stats.h2TipInHeap, stats.h2InvalidTypeKind, stats.h2TipInTim, stats.h2TipNonHeapOk, stats.h3BadRef,
         stats.h3ReachableHolder, stats.h3UnreachableHolder, stats.h3ReachabilityUnknown, stats.h4BadRegion,
         static_cast<unsigned long long>(stats.costNs), maxFailures, stats.samples[0],
         stats.samples[1], stats.samples[2], stats.samples[3]);

    if (UNLIKELY(YyEdgeDiag::Enabled())) {
        YyEdgeDiag::Report(point == nullptr ? "?" : point);
    }

    if (EnvEnabled("MRT_GCV2_VERIFY_HEAP_FATAL") && stats.failures != 0) {
        CHECK_DETAIL(false,
                     "heap object invariant H broken: point=%s failures=%zu objects=%zu H2_tipInHeap=%zu H3=%zu",
                     point == nullptr ? "?" : point, stats.failures, stats.objectsScanned, stats.h2TipInHeap,
                     stats.h3BadRef);
    }
}
} // namespace MapleRuntime
