#include "Heap/Verify/HealPairDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Base/Log.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "ObjectModel/RefField.h"
#include "securec.h"

extern "C" void* CJ_CJThreadGetArg(void);

namespace MapleRuntime {
namespace HealPairDiag {
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
        if (EnvIsOne("MRT_GCV2_HEALPAIR")) {
            return true;
        }
        return DiagGate::TokenOn("healpair");
    }();
    return on;
}

constexpr size_t kPairCap = 1u << 18;
constexpr size_t kCollectCap = 1u << 16;

struct PairRow {
    uintptr_t oldAddr;
    uintptr_t newAddr;
    uintptr_t slot;
    uint16_t site;
    uint16_t phase;
    uint32_t seq;
};

struct CollectRow {
    uintptr_t start;
    uintptr_t end;
    uint64_t liveBytes;
    uint32_t rtype;
    uint32_t knownEmpty;
    uint32_t phase;
    uint32_t nOld;
    uint32_t nNew;
    uint32_t seq;
};

PairRow g_pairs[kPairCap];
CollectRow g_collects[kCollectCap];
std::atomic<uint32_t> g_pairNext{ 0 };
std::atomic<uint32_t> g_collectNext{ 0 };
std::atomic<size_t> g_pairTotal{ 0 };
std::atomic<size_t> g_collectTotal{ 0 };
std::atomic<size_t> g_pairWrap{ 0 };
std::atomic<size_t> g_collectWrap{ 0 };
std::atomic<size_t> g_bySite[4]{ {} };
std::atomic<bool> g_healthOnce{ false };
std::atomic<bool> g_atexitOnce{ false };

int SiteIndex(uint16_t site)
{
    switch (static_cast<HealSite>(site)) {
        case HealSite::WCollectorForwardRawInterior:
            return 0;
        case HealSite::WCollectorPreserveRawInterior:
            return 1;
        case HealSite::WCollectorForwardRawGhost:
            return 2;
        case HealSite::WCollectorNormalizeRawRoot:
            return 3;
        default:
            return -1;
    }
}

const char* SiteName(uint16_t site)
{
    switch (static_cast<HealSite>(site)) {
        case HealSite::WCollectorForwardRawInterior:
            return "WCollectorForwardRawInterior";
        case HealSite::WCollectorPreserveRawInterior:
            return "WCollectorPreserveRawInterior";
        case HealSite::WCollectorForwardRawGhost:
            return "WCollectorForwardRawGhost";
        case HealSite::WCollectorNormalizeRawRoot:
            return "WCollectorNormalizeRawRoot";
        default:
            return "other";
    }
}

void WriteLine(const char* buf, size_t len)
{
    if (buf == nullptr || len == 0) {
        return;
    }
    (void)write(STDERR_FILENO, buf, len);
}

void HealthOnce()
{
    bool expected = false;
    if (g_healthOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        LOG(RTLOG_ERROR, "[GCV2][healpair] health probe_live=1 env=MRT_GCV2_HEALPAIR=1");
    }
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexitOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

uint16_t CurrentPhase()
{
    if (Runtime::CurrentRef() == nullptr) {
        return 0;
    }
    return static_cast<uint16_t>(Heap::GetHeap().GetGCPhase());
}

uint32_t OccupiedPairs()
{
    size_t total = g_pairTotal.load(std::memory_order_acquire);
    return static_cast<uint32_t>(total < kPairCap ? total : kPairCap);
}

void CountInRegion(uintptr_t start, uintptr_t end, uint32_t& nOld, uint32_t& nNew)
{
    nOld = 0;
    nNew = 0;
    uint32_t n = OccupiedPairs();
    uint32_t next = g_pairNext.load(std::memory_order_acquire);
    size_t total = g_pairTotal.load(std::memory_order_acquire);
    uint32_t base = (total < kPairCap) ? 0 : (next % kPairCap);
    for (uint32_t i = 0; i < n; ++i) {
        const PairRow& row = g_pairs[(base + i) % kPairCap];
        if (row.oldAddr >= start && row.oldAddr < end) {
            ++nOld;
        }
        if (row.newAddr >= start && row.newAddr < end) {
            ++nNew;
        }
    }
}

void DumpMatch(const char* tag, const PairRow& row)
{
    char line[384];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][healpair] %s site=%s old=%#zx new=%#zx slot=%#zx phase=%u seq=%u moved=%u\n",
                      tag, SiteName(row.site), row.oldAddr, row.newAddr, row.slot,
                      static_cast<unsigned>(row.phase), row.seq, row.oldAddr != row.newAddr ? 1U : 0U);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

void DumpCollect(const char* tag, const CollectRow& row)
{
    char line[384];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][healpair] %s start=%#zx end=%#zx live=%llu type=%u knownEmpty=%u "
                      "phase=%u nOld=%u nNew=%u seq=%u\n",
                      tag, row.start, row.end, static_cast<unsigned long long>(row.liveBytes),
                      row.rtype, row.knownEmpty, row.phase, row.nOld, row.nNew, row.seq);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

} // namespace

bool Enabled()
{
    return GateOn();
}

void NoteRaw(const void* oldAddr, const void* newAddr, const void* slot, uint16_t site)
{
    if (!GateOn()) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    int idx = SiteIndex(site);
    if (idx >= 0) {
        g_bySite[static_cast<size_t>(idx)].fetch_add(1, std::memory_order_relaxed);
    }
    uint32_t seq = static_cast<uint32_t>(g_pairTotal.fetch_add(1, std::memory_order_relaxed) + 1);
    uint32_t slotIdx = g_pairNext.fetch_add(1, std::memory_order_relaxed);
    if (slotIdx >= kPairCap) {
        g_pairWrap.fetch_add(1, std::memory_order_relaxed);
    }
    PairRow& row = g_pairs[slotIdx % kPairCap];
    row.oldAddr = reinterpret_cast<uintptr_t>(oldAddr);
    row.newAddr = reinterpret_cast<uintptr_t>(newAddr);
    row.slot = reinterpret_cast<uintptr_t>(slot);
    row.site = site;
    row.phase = CurrentPhase();
    row.seq = seq;
}

void NoteCollect(uintptr_t start, uintptr_t end, uint64_t liveBytes, uint32_t rtype, uint32_t knownEmpty)
{
    if (!GateOn() || start == 0 || end <= start) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    uint32_t nOld = 0;
    uint32_t nNew = 0;
    uint32_t seq = static_cast<uint32_t>(g_collectTotal.fetch_add(1, std::memory_order_relaxed) + 1);
    uint32_t slotIdx = g_collectNext.fetch_add(1, std::memory_order_relaxed);
    if (slotIdx >= kCollectCap) {
        g_collectWrap.fetch_add(1, std::memory_order_relaxed);
    }
    CollectRow& row = g_collects[slotIdx % kCollectCap];
    row.start = start;
    row.end = end;
    row.liveBytes = liveBytes;
    row.rtype = rtype;
    row.knownEmpty = knownEmpty;
    row.phase = CurrentPhase();
    row.nOld = nOld;
    row.nNew = nNew;
    row.seq = seq;
}

void NoteCrashRdi(uintptr_t rdi)
{
    if (!GateOn()) {
        return;
    }
    uintptr_t threadObject = 0;
    uintptr_t lwt = 0;
    void* arg = CJ_CJThreadGetArg();
    if (arg != nullptr) {
        lwt = reinterpret_cast<uintptr_t>(arg);
        threadObject = reinterpret_cast<uintptr_t>(static_cast<LWTData*>(arg)->threadObject);
    }
    uint32_t phase = CurrentPhase();
    uint32_t n = OccupiedPairs();
    uint32_t next = g_pairNext.load(std::memory_order_acquire);
    size_t total = g_pairTotal.load(std::memory_order_acquire);
    uint32_t base = (total < kPairCap) ? 0 : (next % kPairCap);

    uint32_t rdiOld = 0;
    uint32_t rdiNew = 0;
    uint32_t thrOld = 0;
    uint32_t thrNew = 0;
    uint32_t lastOldIdx[8];
    uint32_t lastOldN = 0;
    uint32_t firstOldIdx = 0;
    bool haveFirstOld = false;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t idx = (base + i) % kPairCap;
        const PairRow& row = g_pairs[idx];
        if (rdi != 0 && row.oldAddr == rdi) {
            if (!haveFirstOld) {
                firstOldIdx = idx;
                haveFirstOld = true;
            }
            if (lastOldN < 8) {
                lastOldIdx[lastOldN++] = idx;
            } else {
                for (uint32_t k = 1; k < 8; ++k) {
                    lastOldIdx[k - 1] = lastOldIdx[k];
                }
                lastOldIdx[7] = idx;
            }
            ++rdiOld;
        }
        if (rdi != 0 && row.newAddr == rdi) {
            ++rdiNew;
        }
        if (threadObject != 0 && row.oldAddr == threadObject) {
            ++thrOld;
        }
        if (threadObject != 0 && row.newAddr == threadObject) {
            ++thrNew;
        }
    }

    uint32_t collectN = static_cast<uint32_t>(
        g_collectTotal.load(std::memory_order_acquire) < kCollectCap ?
            g_collectTotal.load(std::memory_order_relaxed) : kCollectCap);
    uint32_t collectNext = g_collectNext.load(std::memory_order_acquire);
    size_t collectTotal = g_collectTotal.load(std::memory_order_acquire);
    uint32_t collectBase = (collectTotal < kCollectCap) ? 0 : (collectNext % kCollectCap);
    uint32_t rdiCollect = 0;
    uint32_t lastCollectIdx = 0;
    bool haveCollect = false;
    for (uint32_t i = 0; i < collectN; ++i) {
        uint32_t idx = (collectBase + i) % kCollectCap;
        const CollectRow& row = g_collects[idx];
        if (rdi != 0 && rdi >= row.start && rdi < row.end) {
            ++rdiCollect;
            lastCollectIdx = idx;
            haveCollect = true;
        }
    }

    const char* verdict = "UNRESOLVED";
    if (rdiOld > 0) {
        verdict = "YI";
    } else if (rdiCollect > 0) {
        verdict = "JIA_CANDIDATE";
    }

    char line[768];
    int wn = sprintf_s(line, sizeof(line),
                       "[GCV2][healpair] crash rdi=%#zx threadObject=%#zx equals=%u lwt=%#zx "
                       "phase=%u pairTotal=%zu pairWrap=%zu collectTotal=%zu collectWrap=%zu "
                       "rdiAsOld=%u rdiAsNew=%u thrAsOld=%u thrAsNew=%u rdiCollect=%u "
                       "siteFwdInt=%zu sitePresInt=%zu siteFwdGhost=%zu siteNorm=%zu "
                       "verdict=%s env=MRT_GCV2_HEALPAIR=1\n",
                       rdi, threadObject, (rdi != 0 && rdi == threadObject) ? 1U : 0U, lwt, phase,
                       total, g_pairWrap.load(std::memory_order_relaxed),
                       collectTotal, g_collectWrap.load(std::memory_order_relaxed),
                       rdiOld, rdiNew, thrOld, thrNew, rdiCollect,
                       g_bySite[0].load(std::memory_order_relaxed),
                       g_bySite[1].load(std::memory_order_relaxed),
                       g_bySite[2].load(std::memory_order_relaxed),
                       g_bySite[3].load(std::memory_order_relaxed), verdict);
    if (wn > 0) {
        WriteLine(line, static_cast<size_t>(wn));
    }
    if (haveFirstOld) {
        DumpMatch("firstOld", g_pairs[firstOldIdx]);
    }
    for (uint32_t i = 0; i < lastOldN; ++i) {
        DumpMatch("oldHit", g_pairs[lastOldIdx[i]]);
    }
    if (haveCollect) {
        CollectRow& crow = g_collects[lastCollectIdx];
        CountInRegion(crow.start, crow.end, crow.nOld, crow.nNew);
        DumpCollect("rdiRegion", crow);
    }
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][healpair] report point=%s pairTotal=%zu pairWrap=%zu collectTotal=%zu "
        "collectWrap=%zu siteFwdInt=%zu sitePresInt=%zu siteFwdGhost=%zu siteNorm=%zu "
        "env=MRT_GCV2_HEALPAIR=1",
        point != nullptr ? point : "none", g_pairTotal.load(std::memory_order_relaxed),
        g_pairWrap.load(std::memory_order_relaxed), g_collectTotal.load(std::memory_order_relaxed),
        g_collectWrap.load(std::memory_order_relaxed), g_bySite[0].load(std::memory_order_relaxed),
        g_bySite[1].load(std::memory_order_relaxed), g_bySite[2].load(std::memory_order_relaxed),
        g_bySite[3].load(std::memory_order_relaxed));
}

} // namespace HealPairDiag
} // namespace MapleRuntime
