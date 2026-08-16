#include "Heap/Verify/RegionLifeDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Base/Log.h"
#include "Common/Runtime.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "securec.h"

namespace MapleRuntime {
namespace RegionLifeDiag {
namespace {

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool GateOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        return EnvIsOne("MRT_GCV2_REGIONLIFE") || DiagGate::TokenOn("regionlife");
    }();
    return on;
}

bool WhoZeroOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        return EnvIsOne("MRT_GCV2_WHOZERO") || DiagGate::TokenOn("whozero");
    }();
    return on;
}

void WriteLine(const char* buf, size_t len)
{
    if (buf != nullptr && len > 0) {
        (void)write(STDERR_FILENO, buf, len);
    }
}

// lifeId keyed by region start (unit address). Bumped on Take/Init; stamped on free.
constexpr size_t kLifeCap = 1u << 16;
struct LifeSlot {
    uintptr_t start = 0;
    uint32_t lifeId = 0;
    uint32_t takeSeq = 0;
};
LifeSlot g_life[kLifeCap];
std::atomic<uint32_t> g_takeSeq{ 0 };
std::atomic<uint32_t> g_lifeBump{ 0 };

size_t LifeIdx(uintptr_t start)
{
    return (start >> 12) & (kLifeCap - 1);
}

LifeSlot* FindLife(uintptr_t start, bool insert)
{
    if (start == 0) {
        return nullptr;
    }
    size_t idx = LifeIdx(start);
    for (size_t n = 0; n < 16; ++n) {
        size_t i = (idx + n) & (kLifeCap - 1);
        if (g_life[i].start == start) {
            return &g_life[i];
        }
        if (g_life[i].start == 0) {
            if (!insert) {
                return nullptr;
            }
            g_life[i].start = start;
            g_life[i].lifeId = 0;
            g_life[i].takeSeq = 0;
            return &g_life[i];
        }
    }
    if (!insert) {
        return nullptr;
    }
    // overwrite home
    LifeSlot& s = g_life[idx];
    s.start = start;
    s.lifeId = 0;
    s.takeSeq = 0;
    return &s;
}

constexpr size_t kFreeCap = 1u << 15;
struct FreeRow {
    uintptr_t start = 0;
    uintptr_t end = 0;
    uint32_t freeSeq = 0;
    uint32_t lifeId = 0;
    uint32_t takeSeq = 0;
    uint32_t gcCount = 0;
    uint16_t path = 0;
    uint16_t phase = 0;
    uint64_t liveBytes = 0;
    uint8_t knownEmpty = 0;
    uint8_t young = 0;
    uint8_t neverExam = 0;
    uint8_t auth = 0;
    uint8_t rtype = 0;
    uint8_t large = 0;
};
FreeRow g_frees[kFreeCap];
std::atomic<uint32_t> g_freeNext{ 0 };
std::atomic<size_t> g_freeTotal{ 0 };
std::atomic<size_t> g_freeWrap{ 0 };
std::atomic<size_t> g_takeN{ 0 };
std::atomic<size_t> g_freeN{ 0 };
std::atomic<bool> g_healthOnce{ false };
std::atomic<bool> g_atexitOnce{ false };

thread_local uint16_t t_nextPath = PATH_COLLECT_GENERIC;

uint16_t CurrentPhase()
{
    if (Runtime::CurrentRef() == nullptr) {
        return 0;
    }
    return static_cast<uint16_t>(Heap::GetHeap().GetGCPhase());
}

void HealthOnce()
{
    bool expected = false;
    if (g_healthOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        LOG(RTLOG_ERROR, "[GCV2][regionlife] health probe_live=1 env=MRT_GCV2_REGIONLIFE|WHOZERO");
    }
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexitOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

const char* PathName(uint16_t path)
{
    switch (path) {
        case PATH_FWD_KNOWN_EMPTY:
            return "FwdKnownEmpty";
        case PATH_FWD_AFTER_COPY:
            return "FwdAfterCopy";
        case PATH_PINNED_GARBAGE:
            return "PinnedGarbage";
        case PATH_LARGE_GARBAGE:
            return "LargeGarbage";
        case PATH_RELEASE_LARGE:
            return "ReleaseLarge";
        case PATH_UNUSED_PINNED:
            return "UnusedPinned";
        case PATH_COLLECT_GENERIC:
            return "CollectGeneric";
        default:
            return "Other";
    }
}

void RecordFree(RegionInfo* region, uint16_t path)
{
    if (region == nullptr) {
        return;
    }
    uintptr_t start = region->GetRegionStart();
    uintptr_t end = region->GetRegionEnd();
    if (start == 0 || end <= start) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    LifeSlot* life = FindLife(start, true);
    uint32_t lifeId = life != nullptr ? life->lifeId : 0;
    uint32_t takeSeq = life != nullptr ? life->takeSeq : 0;
    const bool knownEmpty = region->IsKnownEmpty();
    const bool neverExam =
        knownEmpty && region->GetMarkBitmap() == nullptr && region->GetRegionAllocPtr() > region->GetRegionStart();
    uint32_t freeSeq = static_cast<uint32_t>(g_freeTotal.fetch_add(1, std::memory_order_relaxed) + 1);
    uint32_t slotIdx = g_freeNext.fetch_add(1, std::memory_order_relaxed);
    if (slotIdx >= kFreeCap) {
        g_freeWrap.fetch_add(1, std::memory_order_relaxed);
    }
    FreeRow& row = g_frees[slotIdx % kFreeCap];
    row.start = start;
    row.end = end;
    row.freeSeq = freeSeq;
    row.lifeId = lifeId;
    row.takeSeq = takeSeq;
    row.gcCount = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    row.path = path;
    row.phase = CurrentPhase();
    row.liveBytes = region->GetLiveByteCount();
    row.knownEmpty = knownEmpty ? 1 : 0;
    row.young = region->IsYoungRegion() ? 1 : 0;
    row.neverExam = neverExam ? 1 : 0;
    row.auth = region->IsLiveCountAuthoritative() ? 1 : 0;
    row.rtype = static_cast<uint8_t>(region->GetRegionType());
    row.large = region->IsLargeRegion() ? 1 : 0;
    g_freeN.fetch_add(1, std::memory_order_relaxed);
    // Rare-path sample log (bounded).
    if (freeSeq <= 64 || (freeSeq & (freeSeq - 1)) == 0) {
        char line[384];
        int n = sprintf_s(line, sizeof(line),
                          "[GCV2][regionlife] free n=%u path=%s(%u) start=%#zx end=%#zx lifeId=%u takeSeq=%u "
                          "gc=%u phase=%u knownEmpty=%u live=%llu young=%u neverExam=%u auth=%u rtype=%u\n",
                          freeSeq, PathName(path), static_cast<unsigned>(path), start, end, lifeId, takeSeq,
                          row.gcCount, static_cast<unsigned>(row.phase), static_cast<unsigned>(row.knownEmpty),
                          static_cast<unsigned long long>(row.liveBytes), static_cast<unsigned>(row.young),
                          static_cast<unsigned>(row.neverExam), static_cast<unsigned>(row.auth),
                          static_cast<unsigned>(row.rtype));
        if (n > 0) {
            WriteLine(line, static_cast<size_t>(n));
        }
    }
}

} // namespace

bool Enabled()
{
    return GateOn();
}

bool FreeTrackOn()
{
    return GateOn() || WhoZeroOn();
}

void SetNextFreePath(uint16_t path)
{
    t_nextPath = path;
}

uint16_t TakeNextFreePath()
{
    uint16_t p = t_nextPath;
    t_nextPath = PATH_COLLECT_GENERIC;
    return p;
}

void NoteTake(RegionInfo* region)
{
    if (!FreeTrackOn() || region == nullptr) {
        return;
    }
    uintptr_t start = region->GetRegionStart();
    if (start == 0) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    LifeSlot* life = FindLife(start, true);
    if (life == nullptr) {
        return;
    }
    life->lifeId = g_lifeBump.fetch_add(1, std::memory_order_relaxed) + 1;
    life->takeSeq = g_takeSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    g_takeN.fetch_add(1, std::memory_order_relaxed);
}

void NoteFree(RegionInfo* region, uint16_t path)
{
    if (!FreeTrackOn()) {
        return;
    }
    RecordFree(region, path);
}

void NoteRelease(RegionInfo* region, uint16_t path)
{
    if (!FreeTrackOn()) {
        return;
    }
    RecordFree(region, path);
}

bool LookupLastFree(uintptr_t addr, uint32_t* freeSeq, uint32_t* lifeId, uint16_t* path, uint16_t* phase,
                    uint32_t* gcCount, uintptr_t* start, uintptr_t* end, uint8_t* knownEmpty, uint64_t* liveBytes,
                    uint8_t* young, uint8_t* neverExam, uint8_t* auth, uint8_t* sameLife, uint32_t* takeLifeNow)
{
    if (addr == 0 || !FreeTrackOn()) {
        return false;
    }
    uint32_t n = static_cast<uint32_t>(g_freeTotal.load(std::memory_order_acquire) < kFreeCap ?
                                           g_freeTotal.load(std::memory_order_relaxed) :
                                           kFreeCap);
    uint32_t next = g_freeNext.load(std::memory_order_acquire);
    size_t total = g_freeTotal.load(std::memory_order_acquire);
    // Scan newest-first.
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t idx = (next + kFreeCap - 1 - i) % kFreeCap;
        if (total < kFreeCap && idx >= total) {
            continue;
        }
        const FreeRow& row = g_frees[idx];
        if (row.start == 0 || row.end <= row.start) {
            continue;
        }
        if (addr >= row.start && addr < row.end) {
            if (freeSeq != nullptr) {
                *freeSeq = row.freeSeq;
            }
            if (lifeId != nullptr) {
                *lifeId = row.lifeId;
            }
            if (path != nullptr) {
                *path = row.path;
            }
            if (phase != nullptr) {
                *phase = row.phase;
            }
            if (gcCount != nullptr) {
                *gcCount = row.gcCount;
            }
            if (start != nullptr) {
                *start = row.start;
            }
            if (end != nullptr) {
                *end = row.end;
            }
            if (knownEmpty != nullptr) {
                *knownEmpty = row.knownEmpty;
            }
            if (liveBytes != nullptr) {
                *liveBytes = row.liveBytes;
            }
            if (young != nullptr) {
                *young = row.young;
            }
            if (neverExam != nullptr) {
                *neverExam = row.neverExam;
            }
            if (auth != nullptr) {
                *auth = row.auth;
            }
            uint32_t lifeNow = 0;
            LifeSlot* life = FindLife(row.start, false);
            if (life != nullptr) {
                lifeNow = life->lifeId;
            }
            if (takeLifeNow != nullptr) {
                *takeLifeNow = lifeNow;
            }
            // same life: no take after this free for this start (lifeId unchanged).
            if (sameLife != nullptr) {
                *sameLife = (lifeNow == row.lifeId) ? 1 : 0;
            }
            return true;
        }
    }
    return false;
}

void DumpJoinForTarget(uintptr_t tgt, const char* tag)
{
    if (!FreeTrackOn() || tgt == 0) {
        return;
    }
    uint32_t freeSeq = 0;
    uint32_t lifeId = 0;
    uint16_t path = 0;
    uint16_t phase = 0;
    uint32_t gcCount = 0;
    uintptr_t start = 0;
    uintptr_t end = 0;
    uint8_t knownEmpty = 0;
    uint64_t liveBytes = 0;
    uint8_t young = 0;
    uint8_t neverExam = 0;
    uint8_t auth = 0;
    uint8_t sameLife = 0;
    uint32_t takeLifeNow = 0;
    if (!LookupLastFree(tgt, &freeSeq, &lifeId, &path, &phase, &gcCount, &start, &end, &knownEmpty, &liveBytes, &young,
                        &neverExam, &auth, &sameLife, &takeLifeNow)) {
        char miss[160];
        int mn = sprintf_s(miss, sizeof(miss),
                           "[GCV2][regionlife] %s miss tgt=%#zx freeTotal=%zu freeWrap=%zu\n",
                           tag != nullptr ? tag : "join", tgt, g_freeTotal.load(std::memory_order_relaxed),
                           g_freeWrap.load(std::memory_order_relaxed));
        if (mn > 0) {
            WriteLine(miss, static_cast<size_t>(mn));
        }
        return;
    }
    char line[512];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][regionlife] %s tgt=%#zx freeSeq=%u lifeId=%u takeLifeNow=%u sameLife=%u "
                      "path=%s(%u) freePhase=%u freeGc=%u start=%#zx end=%#zx knownEmpty=%u live=%llu "
                      "young=%u neverExam=%u auth=%u (dead_if_knownEmpty=1_means_IsKnownEmpty)\n",
                      tag != nullptr ? tag : "join", tgt, freeSeq, lifeId, takeLifeNow,
                      static_cast<unsigned>(sameLife), PathName(path), static_cast<unsigned>(path),
                      static_cast<unsigned>(phase), gcCount, start, end, static_cast<unsigned>(knownEmpty),
                      static_cast<unsigned long long>(liveBytes), static_cast<unsigned>(young),
                      static_cast<unsigned>(neverExam), static_cast<unsigned>(auth));
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

void Report(const char* point)
{
    if (!FreeTrackOn()) {
        return;
    }
    char line[256];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][regionlife] point=%s takeN=%zu freeN=%zu freeTotal=%zu freeWrap=%zu\n",
                      point != nullptr ? point : "?", g_takeN.load(std::memory_order_relaxed),
                      g_freeN.load(std::memory_order_relaxed), g_freeTotal.load(std::memory_order_relaxed),
                      g_freeWrap.load(std::memory_order_relaxed));
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

} // namespace RegionLifeDiag
} // namespace MapleRuntime
