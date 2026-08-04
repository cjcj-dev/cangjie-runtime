// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "MarkWhyProbe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/ForwardDataManager.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Heap.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"

namespace MapleRuntime {
namespace {

// Always-on stderr: VLOG(REPORT) is gated off by DEFAULT_MRT_REPORT=0.
#define MARKWHY_LOG(fmt, ...)                                                                                          \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][mark-why] " fmt "\n", ##__VA_ARGS__);                                              \
        std::fflush(stderr);                                                                                           \
    } while (0)

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
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v) {
        return def;
    }
    return static_cast<size_t>(n);
}

std::atomic<uint64_t> gN{0};
std::atomic<uint64_t> gOk{0};
std::atomic<uint64_t> gFail{0};
std::atomic<uint64_t> gBmMismatch{0};     // writeBm != readBm
std::atomic<uint64_t> gLiMismatch{0};     // liveInfo changed vs write path (bindedRegion / pointer)
std::atomic<uint64_t> gOffsetOob{0};      // offset beyond bitmap capacity
std::atomic<uint64_t> gNullReadBm{0};
std::atomic<uint64_t> gZeroObjSize{0};
std::atomic<uint64_t> gSmallObjSize{0}; // 0 < size < 8
std::atomic<uint64_t> gAllocEvents{0};
std::atomic<uint64_t> gAllocMulti{0}; // same region saw >1 alloc (approx via last-region race)

// Coarse multi-alloc detector: last region that allocated + count in short window.
std::atomic<uintptr_t> gLastAllocRegion{0};
std::atomic<uint64_t> gLastAllocCount{0};

const char* ThreadRole()
{
    if (IsGcThread()) {
        return "gc";
    }
    if (IsRuntimeThread()) {
        return "runtime";
    }
    return "mutator";
}

void DumpSummaryIfNeeded()
{
    uint64_t n = gN.load(std::memory_order_relaxed);
    if (n == 0 || (n & 0xffff) != 0) {
        return;
    }
    MARKWHY_LOG("SUMMARY n=%llu ok=%llu fail=%llu bm_mismatch=%llu li_mismatch=%llu "
                "oob=%llu null_read_bm=%llu zero_sz=%llu small_sz=%llu alloc_n=%llu alloc_multi=%llu",
                static_cast<unsigned long long>(n), static_cast<unsigned long long>(gOk.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gFail.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gBmMismatch.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gLiMismatch.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gOffsetOob.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gNullReadBm.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gZeroObjSize.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gSmallObjSize.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gAllocEvents.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gAllocMulti.load(std::memory_order_relaxed)));
}

} // namespace

bool MarkWhyProbe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_MARK_WHY");
    return on;
}

bool MarkWhyProbe::AllocTrackEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_MARK_WHY_ALLOC");
    return on;
}

void MarkWhyProbe::NoteMarkBitmapAlloc(RegionInfo* region, RegionBitmap* allocated)
{
    if (!AllocTrackEnabled() || region == nullptr) {
        return;
    }
    static std::atomic<bool> armedLogged{false};
    if (!armedLogged.exchange(true, std::memory_order_relaxed)) {
        MARKWHY_LOG("ARMED_ALLOC env=MRT_GCV2_MARK_WHY_ALLOC=1");
    }
    gAllocEvents.fetch_add(1, std::memory_order_relaxed);
    uintptr_t r = reinterpret_cast<uintptr_t>(region);
    uintptr_t prev = gLastAllocRegion.exchange(r, std::memory_order_relaxed);
    if (prev == r) {
        uint64_t c = gLastAllocCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (c >= 1) {
            gAllocMulti.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<uint64_t> dumpLeft{32};
            uint64_t left = dumpLeft.load(std::memory_order_relaxed);
            if (left > 0 && dumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
                MARKWHY_LOG("MULTI_ALLOC region=%p bm=%p consecutive=%llu start=%#zx type=%u",
                            static_cast<void*>(region), static_cast<void*>(allocated),
                            static_cast<unsigned long long>(c + 1), region->GetRegionStart(),
                            static_cast<unsigned>(region->GetRegionType()));
            }
        }
    } else {
        gLastAllocCount.store(0, std::memory_order_relaxed);
    }
}

bool MarkWhyProbe::NoteAfterMarkBits(RegionInfo* region, const BaseObject* obj, size_t offsetWrite, size_t objSize,
                                     size_t regionSizeArg, RegionBitmap* writeBm, bool markBitsReturnedAlreadyMarked,
                                     const char* site)
{
    if (!Enabled() || region == nullptr) {
        return region != nullptr && region->IsMarkedObject(offsetWrite);
    }
    static std::atomic<bool> armedLogged{false};
    if (!armedLogged.exchange(true, std::memory_order_relaxed)) {
        MARKWHY_LOG("ARMED env=MRT_GCV2_MARK_WHY=1 sample=%zu site=%s", EnvSizeT("MRT_GCV2_MARK_WHY_SAMPLE", 65536),
                    site);
    }

    gN.fetch_add(1, std::memory_order_relaxed);

    MAddress regionStart = region->GetRegionStart();
    MAddress regionEnd = region->GetRegionEnd();
    size_t regionSizeMeta = region->GetRegionSize();
    size_t offsetRecompute =
        (obj != nullptr && reinterpret_cast<MAddress>(obj) >= regionStart)
            ? (reinterpret_cast<MAddress>(obj) - regionStart)
            : static_cast<size_t>(-1);
    bool offsetSame = (offsetWrite == offsetRecompute);

    LiveInfo* liveInfo = region->GetLiveInfo();
    RegionBitmap* readBm = region->GetMarkBitmap();
    bool bmSame = (writeBm == readBm);
    if (!bmSame) {
        gBmMismatch.fetch_add(1, std::memory_order_relaxed);
    }
    if (readBm == nullptr) {
        gNullReadBm.fetch_add(1, std::memory_order_relaxed);
    }
    if (objSize == 0) {
        gZeroObjSize.fetch_add(1, std::memory_order_relaxed);
    } else if (objSize < kMarkedBytesPerBit) {
        gSmallObjSize.fetch_add(1, std::memory_order_relaxed);
    }

    size_t wordCnt = 0;
    size_t bitCapacity = 0; // bits covering region (one bit per 8 bytes)
    if (writeBm != nullptr) {
        wordCnt = writeBm->wordCnt.load(std::memory_order_acquire);
        bitCapacity = wordCnt * kBitsPerWord;
    }
    // offset is byte offset; bit index = offset / 8
    size_t bitIndex = offsetWrite / kMarkedBytesPerBit;
    bool oob = (writeBm != nullptr) && (bitIndex >= bitCapacity || offsetWrite >= regionSizeMeta);
    if (oob) {
        gOffsetOob.fetch_add(1, std::memory_order_relaxed);
    }

    bool markedNow = region->IsMarkedObject(offsetWrite);
    // Direct bit read on writeBm (bypass GetMarkBitmap) to detect identity skew.
    bool markedOnWriteBm = false;
    if (writeBm != nullptr && bitIndex < bitCapacity) {
        markedOnWriteBm = writeBm->IsMarked(offsetWrite);
    }
    bool markedOnReadBm = false;
    if (readBm != nullptr) {
        markedOnReadBm = readBm->IsMarked(offsetWrite);
    }

    if (liveInfo != nullptr && liveInfo->bindedRegion != region) {
        gLiMismatch.fetch_add(1, std::memory_order_relaxed);
    }

    GCPhase phase = Heap::GetHeap().GetCollector().GetGCPhase();
    uint16_t prevTag = ForwardDataManager::GetForwardDataManager().GetPreviousTagID();
    uint16_t tagId = static_cast<uint16_t>(prevTag ^ 1u); // currentTagID = previous ^ 1

    if (markedNow) {
        gOk.fetch_add(1, std::memory_order_relaxed);
    } else {
        gFail.fetch_add(1, std::memory_order_relaxed);
    }

    bool interesting = !markedNow || !bmSame || oob || objSize < kMarkedBytesPerBit || !offsetSame ||
                       (liveInfo != nullptr && liveInfo->bindedRegion != region) ||
                       (markedOnWriteBm != markedOnReadBm);

    static std::atomic<uint64_t> failDumpLeft{128};
    static std::atomic<uint64_t> sampleDumpLeft{32};
    size_t sampleEvery = EnvSizeT("MRT_GCV2_MARK_WHY_SAMPLE", 65536);
    uint64_t n = gN.load(std::memory_order_relaxed);
    bool sampleOk = markedNow && sampleEvery > 0 && (n % sampleEvery) == 0;

    if (interesting) {
        uint64_t left = failDumpLeft.load(std::memory_order_relaxed);
        if (left > 0 && failDumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
            MARKWHY_LOG(
                "FAIL site=%s obj=%p region=%p rStart=%#zx rEnd=%#zx rSize=%zu type=%u young=%u "
                "offsetW=%zu offsetR=%zu offSame=%u objSize=%zu regionSizeArg=%zu "
                "writeBm=%p readBm=%p bmSame=%u wordCnt=%zu bitCap=%zu bitIdx=%zu oob=%u "
                "markedNow=%u markedWriteBm=%u markedReadBm=%u markBitsWasAlready=%u "
                "liveInfo=%p binded=%p liOk=%u phase=%u tagCurGuess=%u prevTag=%u role=%s",
                site, static_cast<const void*>(obj), static_cast<void*>(region), static_cast<uintptr_t>(regionStart),
                static_cast<uintptr_t>(regionEnd), regionSizeMeta, static_cast<unsigned>(region->GetRegionType()),
                region->IsYoungRegion() ? 1u : 0u, offsetWrite, offsetRecompute, offsetSame ? 1u : 0u, objSize,
                regionSizeArg, static_cast<void*>(writeBm), static_cast<void*>(readBm), bmSame ? 1u : 0u, wordCnt,
                bitCapacity, bitIndex, oob ? 1u : 0u, markedNow ? 1u : 0u, markedOnWriteBm ? 1u : 0u,
                markedOnReadBm ? 1u : 0u, markBitsReturnedAlreadyMarked ? 1u : 0u, static_cast<void*>(liveInfo),
                liveInfo != nullptr ? static_cast<void*>(liveInfo->bindedRegion) : nullptr,
                (liveInfo == nullptr || liveInfo->bindedRegion == region) ? 1u : 0u, static_cast<unsigned>(phase),
                static_cast<unsigned>(tagId), static_cast<unsigned>(prevTag), ThreadRole());
        }
    } else if (sampleOk) {
        uint64_t left = sampleDumpLeft.load(std::memory_order_relaxed);
        if (left > 0 && sampleDumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
            MARKWHY_LOG(
                "OK_SAMPLE site=%s obj=%p region=%p rStart=%#zx offsetW=%zu objSize=%zu writeBm=%p readBm=%p "
                "bmSame=%u markedNow=%u phase=%u role=%s",
                site, static_cast<const void*>(obj), static_cast<void*>(region), static_cast<uintptr_t>(regionStart),
                offsetWrite, objSize, static_cast<void*>(writeBm), static_cast<void*>(readBm), bmSame ? 1u : 0u,
                markedNow ? 1u : 0u, static_cast<unsigned>(phase), ThreadRole());
        }
    }

    DumpSummaryIfNeeded();
    return markedNow;
}

} // namespace MapleRuntime
