// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/VerifyHeap.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Common/StateWord.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "ObjectModel/MClass.inline.h"
#include "ObjectModel/RefField.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {
constexpr size_t kSampleLimit = 8;
constexpr size_t kDefaultMaxFailures = 20;

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
//   4) !IsVaildType() → DEFECT (type byte ≥ TYPE_KIND_MAX)
//   5) TypeInfoManager::ContainsAddress(tip) → strongest online positive
//   6) else non-heap + valid type → accept (static TypeInfo in load module)
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
    if (!tip->IsVaildType()) {
        ++stats.h2InvalidTypeKind;
        reason = "invalid-type-kind";
        return HeapVerifyChannel::Defect;
    }
    if (TypeInfoManager::GetTypeInfoManager().ContainsAddress(tipAddr)) {
        ++stats.h2TipInTim;
    } else {
        ++stats.h2TipNonHeapOk;
    }
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

void ReportDefect(HeapVerifyStats& stats, size_t maxFailures, const char* reason, BaseObject* obj,
                  BaseObject* related, int typeByte)
{
    ++stats.failures;
    if (stats.failures > maxFailures) {
        ++stats.truncated;
        return;
    }
    PushSample(stats, obj);
    VLOG(REPORT,
         "[GCV2][verify][heap] BAD_OBJ reason=%s obj=%p related=%p typeByte=%d "
         "failure=%zu max=%zu env=MRT_GCV2_VERIFY_HEAP=1",
         reason, obj, related, typeByte, stats.failures, maxFailures);
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
    // Only sample typeByte when tip is aligned — misaligned tip is not a TypeInfo
    // (typeByte=-128 was a garbage load, not a flag bit; see REPORT-gcvtag).
    if (tip != nullptr && (tipAddr & StateWord::ADDRESS_ALIGN_MASK) == 0) {
        return static_cast<int>(tip->GetType());
    }
    return -1;
}
} // namespace

void VerifyHeapObjects(const char* point, bool force)
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
        [&stats, maxFailures](BaseObject* obj) {
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

            // H3: each ref field null or target satisfies H1+H2.
            if (!obj->HasRefField()) {
                return;
            }
            obj->ForEachRefField([&stats, maxFailures, obj](RefField<>& field) {
                BaseObject* target = field.GetTargetObject();
                if (target == nullptr) {
                    return;
                }
                if (!Heap::IsHeapAddress(target)) {
                    // Non-heap target (e.g. static / foreign) — not invariant H scope.
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
         "H2_tipInTim=%zu H2_tipNonHeapOk=%zu H3_badRef=%zu H4_badRegion=%zu "
         "costNs=%llu maxFailures=%zu "
         "samples=[%p,%p,%p,%p]",
         point == nullptr ? "?" : point, invoke, stats.objectsScanned, stats.failures, stats.infoCount,
         stats.truncated, stats.infoTruncated, stats.h1InvalidObject, stats.h2NullTip, stats.h2MisalignedTip,
         stats.h2TipInHeap, stats.h2InvalidTypeKind, stats.h2TipInTim, stats.h2TipNonHeapOk, stats.h3BadRef,
         stats.h4BadRegion, static_cast<unsigned long long>(stats.costNs), maxFailures, stats.samples[0],
         stats.samples[1], stats.samples[2], stats.samples[3]);

    if (EnvEnabled("MRT_GCV2_VERIFY_HEAP_FATAL") && stats.failures != 0) {
        CHECK_DETAIL(false,
                     "heap object invariant H broken: point=%s failures=%zu objects=%zu H2_tipInHeap=%zu H3=%zu",
                     point == nullptr ? "?" : point, stats.failures, stats.objectsScanned, stats.h2TipInHeap,
                     stats.h3BadRef);
    }
}
} // namespace MapleRuntime
