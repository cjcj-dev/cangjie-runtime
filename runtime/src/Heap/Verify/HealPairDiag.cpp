#include "Heap/Verify/HealPairDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "ObjectModel/MClass.h"
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

uint32_t CountCollectHits(uintptr_t addr, uint32_t& lastIdx, bool& have)
{
    lastIdx = 0;
    have = false;
    if (addr == 0) {
        return 0;
    }
    uint32_t collectN = static_cast<uint32_t>(
        g_collectTotal.load(std::memory_order_acquire) < kCollectCap ?
            g_collectTotal.load(std::memory_order_relaxed) : kCollectCap);
    uint32_t collectNext = g_collectNext.load(std::memory_order_acquire);
    size_t collectTotal = g_collectTotal.load(std::memory_order_acquire);
    uint32_t collectBase = (collectTotal < kCollectCap) ? 0 : (collectNext % kCollectCap);
    uint32_t hits = 0;
    for (uint32_t i = 0; i < collectN; ++i) {
        uint32_t idx = (collectBase + i) % kCollectCap;
        const CollectRow& row = g_collects[idx];
        if (addr >= row.start && addr < row.end) {
            ++hits;
            lastIdx = idx;
            have = true;
        }
    }
    return hits;
}

bool CopyWords(uintptr_t addr, uintptr_t* words, uint32_t nWords)
{
    if (addr == 0 || words == nullptr || nWords == 0) {
        return false;
    }
    std::memcpy(words, reinterpret_cast<const void*>(addr), static_cast<size_t>(nWords) * sizeof(uintptr_t));
    return true;
}

void DumpHolder(const char* tag, uintptr_t addr)
{
    unsigned inHeap = 0;
    unsigned valid = 0;
    unsigned tipInHeap = 0;
    unsigned tipAlign = 0;
    unsigned allZero = 0;
    unsigned nZero = 0;
    unsigned nWords = 0;
    unsigned rtype = 255;
    unsigned young = 0;
    unsigned garbage = 0;
    unsigned freeReg = 0;
    unsigned inAlloc = 0;
    uintptr_t rstart = 0;
    uintptr_t rend = 0;
    uintptr_t alloc = 0;
    uintptr_t header0 = 0;
    uintptr_t header1 = 0;
    uintptr_t tip = 0;
    uintptr_t nameQ = 0;
    uint32_t instSz = 0;
    int typeByte = -1;
    uint8_t flag = 0;
    uint16_t fieldNum = 0;
    char nameBuf[80];
    nameBuf[0] = '\0';
    uintptr_t words[64];
    std::memset(words, 0, sizeof(words));

    if (Runtime::CurrentRef() != nullptr && addr != 0 && Heap::IsHeapAddress(addr)) {
        inHeap = 1;
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(addr);
        if (region != nullptr) {
            rtype = static_cast<unsigned>(region->GetRegionType());
            young = region->IsYoungRegion() ? 1 : 0;
            garbage = region->IsGarbageRegion() ? 1 : 0;
            freeReg = region->IsFreeRegion() ? 1 : 0;
            rstart = region->GetRegionStart();
            rend = region->GetRegionEnd();
            alloc = region->GetRegionAllocPtr();
            inAlloc = (addr >= rstart && addr < alloc) ? 1 : 0;
        }
        uintptr_t limit = rend != 0 ? rend : (addr + sizeof(words));
        nWords = static_cast<unsigned>((limit > addr) ? ((limit - addr) / sizeof(uintptr_t)) : 0);
        if (nWords == 0) {
            nWords = 64;
        }
        if (nWords > 64) {
            nWords = 64;
        }
        if (nWords >= 1 && CopyWords(addr, words, nWords)) {
            header0 = words[0];
            header1 = nWords > 1 ? words[1] : 0;
            unsigned nz = 0;
            for (unsigned i = 0; i < nWords; ++i) {
                if (words[i] == 0) {
                    ++nz;
                }
            }
            nZero = nz;
            allZero = (nz == nWords && nWords > 0) ? 1 : 0;
            for (unsigned row = 0; row < nWords; row += 8) {
                char hex[384];
                int hn = sprintf_s(hex, sizeof(hex),
                                   "[GCV2][slotzero] words tag=%s off=%#x "
                                   "%#zx %#zx %#zx %#zx %#zx %#zx %#zx %#zx\n",
                                   tag, row * 8,
                                   row + 0 < nWords ? words[row + 0] : 0,
                                   row + 1 < nWords ? words[row + 1] : 0,
                                   row + 2 < nWords ? words[row + 2] : 0,
                                   row + 3 < nWords ? words[row + 3] : 0,
                                   row + 4 < nWords ? words[row + 4] : 0,
                                   row + 5 < nWords ? words[row + 5] : 0,
                                   row + 6 < nWords ? words[row + 6] : 0,
                                   row + 7 < nWords ? words[row + 7] : 0);
                if (hn > 0) {
                    WriteLine(hex, static_cast<size_t>(hn));
                }
            }
            tip = header0 & 0xffffffffffffULL;
            valid = (tip != 0) ? 1 : 0;
            if (tip != 0) {
                tipAlign = ((tip & 7U) == 0) ? 1 : 0;
                if (Heap::IsHeapAddress(tip)) {
                    tipInHeap = 1;
                }
                if (tipAlign == 1 && tipInHeap == 0) {
                    uintptr_t tiWords[3];
                    std::memset(tiWords, 0, sizeof(tiWords));
                    if (CopyWords(tip, tiWords, 3)) {
                        nameQ = tiWords[0];
                        const uint8_t* raw = reinterpret_cast<const uint8_t*>(tiWords);
                        typeByte = static_cast<int>(raw[8]);
                        flag = raw[9];
                        fieldNum = static_cast<uint16_t>(raw[10] | (static_cast<uint16_t>(raw[11]) << 8));
                        instSz = static_cast<uint32_t>(raw[12] | (static_cast<uint32_t>(raw[13]) << 8) |
                                                       (static_cast<uint32_t>(raw[14]) << 16) |
                                                       (static_cast<uint32_t>(raw[15]) << 24));
                        if (nameQ != 0 && !Heap::IsHeapAddress(nameQ)) {
                            const char* np = reinterpret_cast<const char*>(nameQ);
                            size_t i = 0;
                            for (; i + 1 < sizeof(nameBuf); ++i) {
                                char c = np[i];
                                if (c == '\0') {
                                    break;
                                }
                                if (c < 32 || c > 126) {
                                    nameBuf[0] = '\0';
                                    i = 0;
                                    break;
                                }
                                nameBuf[i] = c;
                            }
                            nameBuf[i] = '\0';
                        }
                    }
                }
            }
        }
    }

    uint32_t lastCollectIdx = 0;
    bool haveCollect = false;
    uint32_t collectHits = CountCollectHits(addr, lastCollectIdx, haveCollect);

    char line[1024];
    int wn = sprintf_s(line, sizeof(line),
                       "[GCV2][slotzero] holder tag=%s addr=%#zx inHeap=%u valid=%u allZero=%u "
                       "nZero=%u nWords=%u tip=%#zx tipInHeap=%u tipAlign=%u name=%s type=%d "
                       "flag=%u fieldNum=%u instSz=%u header0=%#zx header1=%#zx "
                       "regionType=%u young=%u garbage=%u free=%u inAlloc=%u "
                       "rstart=%#zx rend=%#zx alloc=%#zx collect=%u\n",
                       tag, addr, inHeap, valid, allZero, nZero, nWords, tip, tipInHeap, tipAlign,
                       nameBuf[0] != '\0' ? nameBuf : "?", typeByte, static_cast<unsigned>(flag),
                       static_cast<unsigned>(fieldNum), instSz, header0, header1, rtype, young,
                       garbage, freeReg, inAlloc, rstart, rend, alloc, collectHits);
    if (wn > 0) {
        WriteLine(line, static_cast<size_t>(wn));
    }

    static const unsigned kSlots[] = { 0x08, 0x10, 0x18, 0x20, 0x28, 0xd8, 0xe0, 0xe8,
                                       0x110, 0x118, 0x120, 0x128, 0x130, 0x138,
                                       0x180, 0x188, 0x190 };
    char slotLine[768];
    int pos = sprintf_s(slotLine, sizeof(slotLine), "[GCV2][slotzero] slots tag=%s", tag);
    if (pos > 0) {
        for (unsigned off : kSlots) {
            unsigned wi = off / 8;
            uintptr_t val = (inHeap == 1 && wi < nWords) ? words[wi] : 0;
            unsigned present = (inHeap == 1 && wi < nWords) ? 1 : 0;
            int add = sprintf_s(slotLine + pos, sizeof(slotLine) - static_cast<size_t>(pos),
                                " +%#x=%#zx/%u", off, val, present);
            if (add <= 0) {
                break;
            }
            pos += add;
        }
        int end = sprintf_s(slotLine + pos, sizeof(slotLine) - static_cast<size_t>(pos), "\n");
        if (end > 0) {
            WriteLine(slotLine, static_cast<size_t>(pos + end));
        }
    }
    if (haveCollect) {
        CollectRow& crow = g_collects[lastCollectIdx];
        CountInRegion(crow.start, crow.end, crow.nOld, crow.nNew);
        DumpCollect(tag, crow);
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
    unsigned inHeap = 0;
    if (Runtime::CurrentRef() != nullptr && rdi != 0 && Heap::IsHeapAddress(rdi)) {
        inHeap = 1;
    }
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
    uint32_t lastThrNewIdx = 0;
    bool haveThrNew = false;
    uint32_t lastRdiNewIdx = 0;
    bool haveRdiNew = false;
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
            lastRdiNewIdx = idx;
            haveRdiNew = true;
        }
        if (threadObject != 0 && row.oldAddr == threadObject) {
            ++thrOld;
        }
        if (threadObject != 0 && row.newAddr == threadObject) {
            ++thrNew;
            lastThrNewIdx = idx;
            haveThrNew = true;
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
                       "[GCV2][healpair] crash rdi=%#zx inHeap=%u threadObject=%#zx equals=%u lwt=%#zx "
                       "phase=%u pairTotal=%zu pairWrap=%zu collectTotal=%zu collectWrap=%zu "
                       "rdiAsOld=%u rdiAsNew=%u thrAsOld=%u thrAsNew=%u rdiCollect=%u "
                       "siteFwdInt=%zu sitePresInt=%zu siteFwdGhost=%zu siteNorm=%zu "
                       "verdict=%s env=MRT_GCV2_HEALPAIR=1\n",
                       rdi, inHeap, threadObject, (rdi != 0 && rdi == threadObject) ? 1U : 0U, lwt, phase,
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
    if (haveRdiNew) {
        DumpMatch("rdiNew", g_pairs[lastRdiNewIdx]);
    }
    if (haveThrNew) {
        DumpMatch("thrNew", g_pairs[lastThrNewIdx]);
    }
    if (haveFirstOld) {
        DumpMatch("firstOld", g_pairs[firstOldIdx]);
    }
    for (uint32_t i = 0; i < lastOldN; ++i) {
        DumpMatch("oldHit", g_pairs[lastOldIdx[i]]);
    }
    uint32_t tail = n < 8 ? n : 8;
    for (uint32_t i = 0; i < tail; ++i) {
        uint32_t idx = (base + n - tail + i) % kPairCap;
        DumpMatch("tail", g_pairs[idx]);
    }
    if (haveCollect) {
        CollectRow& crow = g_collects[lastCollectIdx];
        CountInRegion(crow.start, crow.end, crow.nOld, crow.nNew);
        DumpCollect("rdiRegion", crow);
    }
}

void NoteCrashRegs(uintptr_t rdi, uintptr_t rax, uintptr_t r12, uintptr_t r14)
{
    if (!GateOn()) {
        return;
    }
    char line[256];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][slotzero] crashregs rdi=%#zx rax=%#zx r12=%#zx r14=%#zx\n",
                      rdi, rax, r12, r14);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
    DumpHolder("rdi", rdi);
    DumpHolder("rax", rax);
    DumpHolder("r12", r12);
    DumpHolder("r14", r14);
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
