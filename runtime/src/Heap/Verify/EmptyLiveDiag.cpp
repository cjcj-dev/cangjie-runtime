// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/EmptyLiveDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"

namespace MapleRuntime {
namespace EmptyLiveDiag {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

bool GateOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        if (EnvIsOne("MRT_GCV2_EMPTYLIVE")) {
            return true;
        }
        return DiagGate::TokenOn("emptylive");
    }();
    return on;
}

constexpr size_t kSampleCap = 64;
constexpr size_t kMaxSteps = 4096;

std::atomic<size_t> g_enter{ 0 };
std::atomic<size_t> g_knownEmpty{ 0 };
std::atomic<size_t> g_notEmpty{ 0 };

// knownEmpty size-walk aggregates
std::atomic<size_t> g_keValid{ 0 };
std::atomic<size_t> g_keEpochEq{ 0 };     // IsMarkedObject true (epoch match + bit)
std::atomic<size_t> g_keRawBit{ 0 };      // raw markWords bit regardless of epoch
std::atomic<size_t> g_keEpochStaleBit{ 0 }; // raw bit set but face epoch != snapshot
std::atomic<size_t> g_keEpochEqBit{ 0 };   // raw bit set and face epoch == snapshot
std::atomic<size_t> g_keNoFaceBit{ 0 };    // raw bit path unavailable (no face/bitmap)
std::atomic<size_t> g_keRegionsRawGt0{ 0 };
std::atomic<size_t> g_keRegionsEpochEqGt0{ 0 };
std::atomic<size_t> g_keRegionsStaleBitGt0{ 0 };

// positive control: !knownEmpty
std::atomic<size_t> g_posValid{ 0 };
std::atomic<size_t> g_posEpochEq{ 0 };
std::atomic<size_t> g_posRawBit{ 0 };
std::atomic<size_t> g_posEpochStaleBit{ 0 };
std::atomic<size_t> g_posRegionsEpochEqGt0{ 0 };
std::atomic<size_t> g_posSampled{ 0 };

std::atomic<size_t> g_sampleLogged{ 0 };
std::atomic<size_t> g_posSampleLogged{ 0 };
std::atomic<bool> g_atexit{ false };

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

// Size-walk: count valid headers, epoch-gated marks, raw bits, and split raw by epoch equality.
void CountMarksEpochSplit(RegionInfo* region, size_t& validOut, size_t& epochEqOut, size_t& rawBitOut,
                          size_t& staleBitOut, size_t& eqBitOut, size_t& noFaceBitOut, uint64_t& faceEpochOut,
                          uint64_t& snapEpochOut, int& faceStateOut)
{
    validOut = 0;
    epochEqOut = 0;
    rawBitOut = 0;
    staleBitOut = 0;
    eqBitOut = 0;
    noFaceBitOut = 0;
    faceEpochOut = 0;
    snapEpochOut = 0;
    faceStateOut = -1; // -1 none, 0 stale, 1 current, 2 large

    if (region == nullptr) {
        return;
    }
    snapEpochOut = region->GetSnapshotEpoch();

    if (region->IsLargeRegion()) {
        faceStateOut = 2;
        validOut = 1;
        const bool ep = region->IsMarkedObject(static_cast<size_t>(0));
        if (ep) {
            ++epochEqOut;
            ++rawBitOut;
            ++eqBitOut;
        }
        return;
    }

    LiveInfo* liveInfo = region->GetLiveInfo();
    RegionBitmap* mb = nullptr;
    if (liveInfo != nullptr) {
        faceEpochOut = liveInfo->markEpoch;
        mb = __atomic_load_n(&liveInfo->markBitmap, std::memory_order_acquire);
        if (reinterpret_cast<MAddress>(mb) == LiveInfo::TEMPORARY_PTR) {
            mb = nullptr;
        }
        faceStateOut = (liveInfo->markEpoch == snapEpochOut) ? 1 : 0;
    }

    size_t start = region->GetRegionStart();
    size_t alloc = region->GetRegionAllocPtr();
    if (alloc <= start) {
        return;
    }
    uintptr_t pos = start;
    size_t steps = 0;
    while (pos < alloc && steps < kMaxSteps) {
        BaseObject* o = from_region_addr(pos);
        if (!Collector::PlausibleManagedObjectGate("emptylive-count", o)) {
            break;
        }
        size_t sz = o->GetSize();
        if (sz == 0) {
            break;
        }
        ++validOut;
        if (region->IsMarkedObject(o)) {
            ++epochEqOut;
        }
        if (mb != nullptr) {
            size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(o));
            if (mb->IsMarked(offset)) {
                ++rawBitOut;
                if (faceStateOut == 1) {
                    ++eqBitOut;
                } else if (faceStateOut == 0) {
                    ++staleBitOut;
                }
            }
        } else {
            // no face/bitmap: cannot attribute raw bit; still track for self-check
            ++noFaceBitOut;
        }
        pos += sz;
        ++steps;
    }
    // noFaceBitOut counted per object when no bitmap — only meaningful as "walked without face"
    if (mb != nullptr) {
        noFaceBitOut = 0;
    }
}

} // namespace

bool Enabled()
{
    return GateOn();
}

void NoteCollectEnter(RegionInfo* region)
{
    if (!GateOn()) {
        return;
    }
    EnsureAtexit();
    size_t n = g_enter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (region == nullptr) {
        return;
    }

    const bool knownEmpty = region->IsKnownEmpty();
    if (knownEmpty) {
        g_knownEmpty.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_notEmpty.fetch_add(1, std::memory_order_relaxed);
    }

    // Full count on knownEmpty; sample first N and every 64th non-empty as positive control.
    const bool doCount = knownEmpty || (n <= kSampleCap) || ((n & 63u) == 0);
    if (!doCount) {
        return;
    }

    size_t valid = 0;
    size_t epochEq = 0;
    size_t rawBit = 0;
    size_t staleBit = 0;
    size_t eqBit = 0;
    size_t noFace = 0;
    uint64_t faceEp = 0;
    uint64_t snapEp = 0;
    int faceSt = -1;
    CountMarksEpochSplit(region, valid, epochEq, rawBit, staleBit, eqBit, noFace, faceEp, snapEp, faceSt);

    if (knownEmpty) {
        g_keValid.fetch_add(valid, std::memory_order_relaxed);
        g_keEpochEq.fetch_add(epochEq, std::memory_order_relaxed);
        g_keRawBit.fetch_add(rawBit, std::memory_order_relaxed);
        g_keEpochStaleBit.fetch_add(staleBit, std::memory_order_relaxed);
        g_keEpochEqBit.fetch_add(eqBit, std::memory_order_relaxed);
        g_keNoFaceBit.fetch_add(noFace, std::memory_order_relaxed);
        if (rawBit > 0) {
            g_keRegionsRawGt0.fetch_add(1, std::memory_order_relaxed);
        }
        if (epochEq > 0) {
            g_keRegionsEpochEqGt0.fetch_add(1, std::memory_order_relaxed);
        }
        if (staleBit > 0) {
            g_keRegionsStaleBitGt0.fetch_add(1, std::memory_order_relaxed);
        }
        size_t slog = g_sampleLogged.fetch_add(1, std::memory_order_relaxed);
        if (slog < kSampleCap) {
            std::fprintf(stderr,
                         "[GCV2][emptylive][ke] n=%zu region=%p start=%#zx alloc=%#zx type=%u route=%u "
                         "live=%llu auth=%u knownEmpty=1 faceSt=%d faceEp=%llu snapEp=%llu "
                         "valid=%zu epochEq=%zu rawBit=%zu staleBit=%zu eqBit=%zu noFaceWalk=%zu "
                         "bitmap=%p\n",
                         n, region, region->GetRegionStart(), region->GetRegionAllocPtr(),
                         static_cast<unsigned>(region->GetRegionType()),
                         static_cast<unsigned>(region->GetRouteState()),
                         static_cast<unsigned long long>(region->GetLiveByteCount()),
                         static_cast<unsigned>(region->IsLiveCountAuthoritative()), faceSt,
                         static_cast<unsigned long long>(faceEp), static_cast<unsigned long long>(snapEp), valid,
                         epochEq, rawBit, staleBit, eqBit, noFace, region->GetMarkBitmap());
            std::fflush(stderr);
        }
    } else {
        g_posSampled.fetch_add(1, std::memory_order_relaxed);
        g_posValid.fetch_add(valid, std::memory_order_relaxed);
        g_posEpochEq.fetch_add(epochEq, std::memory_order_relaxed);
        g_posRawBit.fetch_add(rawBit, std::memory_order_relaxed);
        g_posEpochStaleBit.fetch_add(staleBit, std::memory_order_relaxed);
        if (epochEq > 0) {
            g_posRegionsEpochEqGt0.fetch_add(1, std::memory_order_relaxed);
        }
        size_t slog = g_posSampleLogged.fetch_add(1, std::memory_order_relaxed);
        if (slog < 16) {
            std::fprintf(stderr,
                         "[GCV2][emptylive][pos] n=%zu region=%p live=%llu auth=%u knownEmpty=0 faceSt=%d "
                         "faceEp=%llu snapEp=%llu valid=%zu epochEq=%zu rawBit=%zu staleBit=%zu\n",
                         n, region, static_cast<unsigned long long>(region->GetLiveByteCount()),
                         static_cast<unsigned>(region->IsLiveCountAuthoritative()), faceSt,
                         static_cast<unsigned long long>(faceEp), static_cast<unsigned long long>(snapEp), valid,
                         epochEq, rawBit, staleBit);
            std::fflush(stderr);
        }
    }
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    const size_t enter = g_enter.load(std::memory_order_relaxed);
    const size_t ke = g_knownEmpty.load(std::memory_order_relaxed);
    const size_t ne = g_notEmpty.load(std::memory_order_relaxed);
    const size_t keValid = g_keValid.load(std::memory_order_relaxed);
    const size_t keEp = g_keEpochEq.load(std::memory_order_relaxed);
    const size_t keRaw = g_keRawBit.load(std::memory_order_relaxed);
    const size_t keStale = g_keEpochStaleBit.load(std::memory_order_relaxed);
    const size_t keEqBit = g_keEpochEqBit.load(std::memory_order_relaxed);
    const size_t keNoFace = g_keNoFaceBit.load(std::memory_order_relaxed);
    const size_t keRawR = g_keRegionsRawGt0.load(std::memory_order_relaxed);
    const size_t keEpR = g_keRegionsEpochEqGt0.load(std::memory_order_relaxed);
    const size_t keStaleR = g_keRegionsStaleBitGt0.load(std::memory_order_relaxed);
    const size_t posN = g_posSampled.load(std::memory_order_relaxed);
    const size_t posValid = g_posValid.load(std::memory_order_relaxed);
    const size_t posEp = g_posEpochEq.load(std::memory_order_relaxed);
    const size_t posRaw = g_posRawBit.load(std::memory_order_relaxed);
    const size_t posStale = g_posEpochStaleBit.load(std::memory_order_relaxed);
    const size_t posEpR = g_posRegionsEpochEqGt0.load(std::memory_order_relaxed);

    // Self-check: for knownEmpty with a face, staleBit+eqBit should equal rawBit.
    const size_t splitSum = keStale + keEqBit;
    const unsigned splitOk = (splitSum == keRaw) ? 1u : 0u;

    std::fprintf(stderr,
                 "[GCV2][emptylive] point=%s enter=%zu knownEmpty=%zu notEmpty=%zu "
                 "ke_valid=%zu ke_epochEq=%zu ke_rawBit=%zu ke_staleBit=%zu ke_eqBit=%zu "
                 "ke_noFaceWalk=%zu ke_regions_raw_gt0=%zu ke_regions_epochEq_gt0=%zu "
                 "ke_regions_staleBit_gt0=%zu split_stale_plus_eq=%zu split_ok=%u "
                 "pos_sampled=%zu pos_valid=%zu pos_epochEq=%zu pos_rawBit=%zu pos_staleBit=%zu "
                 "pos_regions_epochEq_gt0=%zu\n",
                 point != nullptr ? point : "?", enter, ke, ne, keValid, keEp, keRaw, keStale, keEqBit, keNoFace,
                 keRawR, keEpR, keStaleR, splitSum, splitOk, posN, posValid, posEp, posRaw, posStale, posEpR);
    std::fflush(stderr);
}

} // namespace EmptyLiveDiag
} // namespace MapleRuntime
