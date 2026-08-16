#include "Heap/Verify/HealPairDiag.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Base/Log.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/GateDropDiag.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Verify/NoTracedDiag.h"
#include "Heap/Verify/RegionLifeDiag.h"
#include "Heap/Verify/TraceClear.h"
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

// whozero reuses the zero-write ring + crash match; default off.
bool WhoZeroOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        if (EnvIsOne("MRT_GCV2_WHOZERO")) {
            return true;
        }
        return DiagGate::TokenOn("whozero");
    }();
    return on;
}

// edgemiss: non-zero write ring for large targets + first-mark ring. Default off.
bool EdgeMissOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        if (EnvIsOne("MRT_GCV2_EDGEMISS")) {
            return true;
        }
        if (EnvIsOne("MRT_GCV2_WHOZERO")) {
            return true;
        }
        return DiagGate::TokenOn("edgemiss") || DiagGate::TokenOn("whozero");
    }();
    return on;
}

bool ZeroTrackOn()
{
    return GateOn() || WhoZeroOn();
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

constexpr size_t kCopyCap = 1u << 16;
constexpr size_t kZeroCap = 1u << 14;

struct CopyRow {
    uintptr_t from;
    uintptr_t to;
    uint32_t size;
    uint16_t phase;
    uint16_t done;
    uint32_t seq;
};

// noforward: site-52/42 CAS-null snapshot of the *target* (oldRaw peel) + holder gen.
// classKind: 1=甲 unmarked 2=乙 free|garbage 3=丙 marked no fwd 4=丁 not in cset/ghost 0=n/a
// majoryoung holder face (CAS-null time): myBranch 1=甲 2=乙 3=丙 0=n/a
//   乙 = holderMarked=1 (edge miss into neverExam target)
//   丙 = holderMarked=0 && holder region still live (young/large reclaim inconsistency)
//   甲 = holderMarked=0 && holder free|garbage (holder also judged dead)
struct ZeroRow {
    uintptr_t slot;
    uintptr_t oldRaw;
    uint16_t site;
    uint16_t phase;
    uint32_t seq;
    uintptr_t tgt;
    uint32_t fwdWord;
    uint8_t rtype;
    uint8_t routeState;
    uint8_t freeR;
    uint8_t garbR;
    uint8_t youngR;
    uint8_t ghostR;
    uint8_t marked;
    uint8_t live0;
    uint8_t holderYoung;
    uint8_t classKind;
    uintptr_t holderObj;
    uintptr_t holderStart;
    uint8_t holderMarked;
    uint8_t holderMb;
    uint8_t holderFree;
    uint8_t holderGarb;
    uint8_t holderAuth;
    uint8_t holderLive0;
    uint8_t holderNeverExam;
    uint8_t myBranch;
    uint8_t holderRtype;
    uint8_t holderResolved;
};

// Page filter for whozero zero-slot remapping on copy (4KiB pages). Default unused until WHOZERO.
constexpr size_t kZeroPageBits = 1u << 20; // 1M bits ~ 4 TiB @ 4KiB
constexpr uintptr_t kZeroPageShift = 12;
uint8_t g_zeroPageBits[kZeroPageBits / 8] = {};

inline void MarkZeroPage(uintptr_t slot)
{
    if (slot == 0) {
        return;
    }
    size_t bit = (slot >> kZeroPageShift) & (kZeroPageBits - 1);
    g_zeroPageBits[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
}

inline bool ZeroPageMarked(uintptr_t addr)
{
    if (addr == 0) {
        return false;
    }
    size_t bit = (addr >> kZeroPageShift) & (kZeroPageBits - 1);
    return (g_zeroPageBits[bit / 8] & static_cast<uint8_t>(1u << (bit % 8))) != 0;
}

inline bool ZeroRangeMaybeMarked(uintptr_t from, uintptr_t size)
{
    if (from == 0 || size == 0) {
        return false;
    }
    uintptr_t end = from + size - 1;
    uintptr_t p = from & ~((uintptr_t(1) << kZeroPageShift) - 1);
    uintptr_t pEnd = end & ~((uintptr_t(1) << kZeroPageShift) - 1);
    for (;;) {
        if (ZeroPageMarked(p)) {
            return true;
        }
        if (p >= pEnd) {
            return false;
        }
        p += (uintptr_t(1) << kZeroPageShift);
    }
}

struct InflightCopy {
    uintptr_t from;
    uintptr_t to;
    uint32_t size;
    uint16_t phase;
};

CopyRow g_copies[kCopyCap];
ZeroRow g_zeros[kZeroCap];
std::atomic<uint32_t> g_copyNext{ 0 };
std::atomic<uint32_t> g_zeroNext{ 0 };
std::atomic<size_t> g_copyTotal{ 0 };
std::atomic<size_t> g_zeroTotal{ 0 };
std::atomic<size_t> g_copyWrap{ 0 };
std::atomic<size_t> g_zeroWrap{ 0 };
std::atomic<size_t> g_zeroRemap{ 0 };
thread_local InflightCopy g_inflight{};

// edgemiss rings (narrow: large target / first mark). Remap slots on copy.
constexpr size_t kEdgeCap = 1u << 16; // larger: null→large only is sparse
constexpr size_t kMarkCap = 1u << 16;
struct EdgeRow {
    uintptr_t slot = 0;
    uintptr_t oldRaw = 0;
    uintptr_t newRaw = 0;
    uintptr_t tgt = 0;
    uintptr_t holder = 0;
    uintptr_t pc = 0;
    uint32_t gcCount = 0;
    uint32_t seq = 0;
    uint16_t phase = 0;
    uint8_t barrier = 0;
    uint8_t satbOld = 0;
    uint8_t satbNew = 0;
    uint8_t large = 0;
    uint8_t holderMarked = 0xff;
    uint8_t holderYoung = 0xff;
};
struct MarkRow {
    uintptr_t obj = 0;
    uintptr_t start = 0;
    uint32_t off = 0;
    uint32_t gcCount = 0;
    uint32_t seq = 0;
    uint16_t phase = 0;
};
EdgeRow g_edges[kEdgeCap];
MarkRow g_marks[kMarkCap];
std::atomic<uint32_t> g_edgeNext{ 0 };
std::atomic<uint32_t> g_markNext{ 0 };
std::atomic<size_t> g_edgeTotal{ 0 };
std::atomic<size_t> g_markTotal{ 0 };
std::atomic<size_t> g_edgeWrap{ 0 };
std::atomic<size_t> g_markWrap{ 0 };
std::atomic<size_t> g_edgeRemap{ 0 };
std::atomic<size_t> g_markRemap{ 0 };
// Page filter for edge-slot remap (share zero-page style).
uint8_t g_edgePageBits[kZeroPageBits / 8] = {};
inline void MarkEdgePage(uintptr_t slot)
{
    if (slot == 0) {
        return;
    }
    size_t bit = (slot >> kZeroPageShift) & (kZeroPageBits - 1);
    g_edgePageBits[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
}
inline bool EdgePageMarked(uintptr_t addr)
{
    if (addr == 0) {
        return false;
    }
    size_t bit = (addr >> kZeroPageShift) & (kZeroPageBits - 1);
    return (g_edgePageBits[bit / 8] & static_cast<uint8_t>(1u << (bit % 8))) != 0;
}
inline bool EdgeRangeMaybeMarked(uintptr_t from, uintptr_t size)
{
    if (from == 0 || size == 0) {
        return false;
    }
    uintptr_t end = from + size - 1;
    uintptr_t p = from & ~((uintptr_t(1) << kZeroPageShift) - 1);
    uintptr_t pEnd = end & ~((uintptr_t(1) << kZeroPageShift) - 1);
    for (;;) {
        if (EdgePageMarked(p)) {
            return true;
        }
        if (p >= pEnd) {
            return false;
        }
        p += (uintptr_t(1) << kZeroPageShift);
    }
}
const char* BarrierName(uint8_t b)
{
    switch (b) {
        case 1:
            return "Idle";
        case 2:
            return "Enum";
        case 3:
            return "Trace";
        case 4:
            return "PostTrace";
        case 5:
            return "Preforward";
        case 6:
            return "Forward";
        case 7:
            return "STW";
        default:
            return "unknown";
    }
}
bool PhaseSatbActive(uint16_t phase)
{
    return phase == static_cast<uint16_t>(GCPhase::GC_PHASE_ENUM) ||
        phase == static_cast<uint16_t>(GCPhase::GC_PHASE_TRACE) ||
        phase == static_cast<uint16_t>(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
}

// slotwindow inflight=0: g_inflight is TLS. Crash dump runs on the mutator,
// so it never sees the GC thread's mid-copy. This table is the cross-thread view.
constexpr size_t kGlobalInflightCap = 64;
struct GlobalInflight {
    std::atomic<uintptr_t> from{ 0 };
    std::atomic<uintptr_t> to{ 0 };
    std::atomic<uint32_t> size{ 0 };
    std::atomic<uint16_t> phase{ 0 };
    std::atomic<uint16_t> live{ 0 };
};
GlobalInflight g_globalInflight[kGlobalInflightCap];
thread_local int g_globalSlot = -1;

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
        case HealSite::WCollectorMinorFixForwardNull:
            return "WCollectorMinorFixForwardNull";
        case HealSite::WCollectorMinorResolveDead:
            return "WCollectorMinorResolveDead";
        case HealSite::WCollectorFixOldTaggedDead:
            return "WCollectorFixOldTaggedDead";
        case HealSite::WCollectorRemsetResolveDead:
            return "WCollectorRemsetResolveDead";
        case HealSite::WCollectorResolveDeadRoot:
            return "WCollectorResolveDeadRoot";
        case HealSite::BaseObjectCompareExchangeRefField:
            return "BaseObjectCompareExchangeRefField";
        case HealSite::IdleReadReference:
            return "IdleReadReference";
        case HealSite::PreforwardReadReference:
            return "PreforwardReadReference";
        case HealSite::ForwardReadReference:
            return "ForwardReadReference";
        case HealSite::PostTraceReadReference:
            return "PostTraceReadReference";
        case HealSite::EnumReadReference:
            return "EnumReadReference";
        case HealSite::TraceReadReference:
            return "TraceReadReference";
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

void DumpCopy(const char* tag, const CopyRow& row)
{
    char line[384];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][slotwindow] %s from=%#zx to=%#zx size=%u phase=%u done=%u seq=%u\n",
                      tag, row.from, row.to, row.size, static_cast<unsigned>(row.phase),
                      static_cast<unsigned>(row.done), row.seq);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

// Join edge-write (T1) + first-mark (T2) + regionlife free (T3) for a whozero slot/target.
void DumpEdgeJoin(uintptr_t slot, uintptr_t holder, uintptr_t tgt, const char* tag)
{
    if (!EdgeMissOn()) {
        return;
    }
    uint32_t en = static_cast<uint32_t>(
        g_edgeTotal.load(std::memory_order_acquire) < kEdgeCap ?
            g_edgeTotal.load(std::memory_order_relaxed) : kEdgeCap);
    uint32_t enext = g_edgeNext.load(std::memory_order_acquire);
    size_t etot = g_edgeTotal.load(std::memory_order_acquire);
    uint32_t ebase = (etot < kEdgeCap) ? 0 : (enext % kEdgeCap);
    int lastEi = -1;
    uint32_t edgeHits = 0;
    for (uint32_t i = 0; i < en; ++i) {
        const EdgeRow& er = g_edges[(ebase + i) % kEdgeCap];
        bool hit = (slot != 0 && er.slot == slot);
        if (!hit && tgt != 0 && er.tgt == tgt && holder != 0 && er.holder == holder) {
            hit = true;
        }
        if (hit) {
            ++edgeHits;
            lastEi = static_cast<int>((ebase + i) % kEdgeCap);
        }
    }
    uint32_t mn = static_cast<uint32_t>(
        g_markTotal.load(std::memory_order_acquire) < kMarkCap ?
            g_markTotal.load(std::memory_order_relaxed) : kMarkCap);
    uint32_t mnext = g_markNext.load(std::memory_order_acquire);
    size_t mtot = g_markTotal.load(std::memory_order_acquire);
    uint32_t mbase = (mtot < kMarkCap) ? 0 : (mnext % kMarkCap);
    int lastMi = -1;
    uint32_t markHits = 0;
    if (holder != 0) {
        for (uint32_t i = 0; i < mn; ++i) {
            const MarkRow& mr = g_marks[(mbase + i) % kMarkCap];
            if (mr.obj == holder) {
                ++markHits;
                lastMi = static_cast<int>((mbase + i) % kMarkCap);
            }
        }
    }
    uint32_t freeSeq = 0;
    uint32_t freeLife = 0;
    uint16_t freePath = 0;
    uint16_t freePhase = 0;
    uint32_t freeGc = 0;
    uintptr_t freeStart = 0;
    uintptr_t freeEnd = 0;
    uint8_t freeKnown = 0;
    uint64_t freeLive = 0;
    uint8_t freeYoung = 0;
    uint8_t freeNever = 0;
    uint8_t freeAuth = 0;
    uint8_t freeSame = 0;
    uint32_t takeLife = 0;
    unsigned haveFree = 0;
    if (tgt != 0) {
        haveFree = RegionLifeDiag::LookupLastFree(tgt, &freeSeq, &freeLife, &freePath, &freePhase, &freeGc, &freeStart,
                                                  &freeEnd, &freeKnown, &freeLive, &freeYoung, &freeNever, &freeAuth,
                                                  &freeSame, &takeLife)
                       ? 1U
                       : 0U;
    }
    unsigned verdict = 0;
    const char* verdictName = "na";
    if (lastEi < 0) {
        verdict = 1;
        verdictName = "jia_no_t1";
    } else if (lastMi < 0) {
        verdict = 4;
        verdictName = "no_t2_mark";
    } else {
        const EdgeRow& er = g_edges[static_cast<size_t>(lastEi)];
        const MarkRow& mr = g_marks[static_cast<size_t>(lastMi)];
        if (er.holderMarked == 1) {
            verdict = 2;
            verdictName = "yi_post_mark";
        } else if (er.gcCount < mr.gcCount) {
            verdict = 3;
            verdictName = "bing_pre_mark";
        } else if (er.gcCount > mr.gcCount) {
            verdict = 2;
            verdictName = "yi_post_mark_gc";
        } else if (er.phase < mr.phase) {
            verdict = 3;
            verdictName = "bing_pre_mark_phase";
        } else if (er.phase > mr.phase) {
            verdict = 2;
            verdictName = "yi_post_mark_phase";
        } else {
            verdict = 2;
            verdictName = "yi_same_phase_conc";
        }
    }
    char line[768];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][edgemiss] join tag=%s slot=%#zx holder=%#zx tgt=%#zx "
                      "haveT1=%u edgeHits=%u edgeTotal=%zu edgeRemap=%zu "
                      "haveT2=%u markHits=%u markTotal=%zu "
                      "haveT3=%u freePath=%u freePhase=%u freeGc=%u neverExam=%u sameLife=%u "
                      "verdict=%s(%u)\n",
                      tag != nullptr ? tag : "?", slot, holder, tgt, lastEi >= 0 ? 1U : 0U, edgeHits, etot,
                      g_edgeRemap.load(std::memory_order_relaxed), lastMi >= 0 ? 1U : 0U, markHits, mtot, haveFree,
                      static_cast<unsigned>(freePath), static_cast<unsigned>(freePhase), freeGc,
                      static_cast<unsigned>(freeNever), static_cast<unsigned>(freeSame), verdictName, verdict);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
    if (lastEi >= 0) {
        const EdgeRow& er = g_edges[static_cast<size_t>(lastEi)];
        char e[640];
        int en2 = sprintf_s(e, sizeof(e),
                            "[GCV2][edgemiss] T1 write slot=%#zx tgt=%#zx holder=%#zx phase=%u gc=%u "
                            "barrier=%s(%u) satbOld=%u satbNew=%u hMarked@w=%u hYoung=%u seq=%u pc=%#zx\n",
                            er.slot, er.tgt, er.holder, static_cast<unsigned>(er.phase), er.gcCount,
                            BarrierName(er.barrier), static_cast<unsigned>(er.barrier),
                            static_cast<unsigned>(er.satbOld), static_cast<unsigned>(er.satbNew),
                            static_cast<unsigned>(er.holderMarked), static_cast<unsigned>(er.holderYoung), er.seq,
                            er.pc);
        if (en2 > 0) {
            WriteLine(e, static_cast<size_t>(en2));
        }
    } else {
        char miss[256];
        int mn2 = sprintf_s(miss, sizeof(miss),
                            "[GCV2][edgemiss] T1_missing slot=%#zx tgt=%#zx edgeTotal=%zu "
                            "(no barrier NoteEdgeWrite for large tgt; not proof of plain store alone)\n",
                            slot, tgt, etot);
        if (mn2 > 0) {
            WriteLine(miss, static_cast<size_t>(mn2));
        }
    }
    if (lastMi >= 0) {
        const MarkRow& mr = g_marks[static_cast<size_t>(lastMi)];
        char m[320];
        int mn3 = sprintf_s(m, sizeof(m),
                            "[GCV2][edgemiss] T2 mark obj=%#zx start=%#zx off=%u phase=%u gc=%u seq=%u\n",
                            mr.obj, mr.start, mr.off, static_cast<unsigned>(mr.phase), mr.gcCount, mr.seq);
        if (mn3 > 0) {
            WriteLine(m, static_cast<size_t>(mn3));
        }
    }
    if (haveFree) {
        char f[320];
        int fn = sprintf_s(f, sizeof(f),
                           "[GCV2][edgemiss] T3 free path=%u phase=%u gc=%u start=%#zx neverExam=%u sameLife=%u\n",
                           static_cast<unsigned>(freePath), static_cast<unsigned>(freePhase), freeGc, freeStart,
                           static_cast<unsigned>(freeNever), static_cast<unsigned>(freeSame));
        if (fn > 0) {
            WriteLine(f, static_cast<size_t>(fn));
        }
    }
}

void DumpZero(const char* tag, const ZeroRow& row)
{
    char line[768];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][slotwindow] %s slot=%#zx old=%#zx site=%u phase=%u seq=%u "
                      "tgt=%#zx rtype=%u rs=%u free=%u garb=%u young=%u ghost=%u "
                      "marked=%u live0=%u fwd=%#x holderYoung=%u class=%u "
                      "hObj=%#zx hStart=%#zx hMarked=%u hMb=%u hFree=%u hGarb=%u "
                      "hAuth=%u hLive0=%u hNeverExam=%u hRtype=%u hRes=%u myBranch=%u\n",
                      tag, row.slot, row.oldRaw, static_cast<unsigned>(row.site),
                      static_cast<unsigned>(row.phase), row.seq, row.tgt,
                      static_cast<unsigned>(row.rtype), static_cast<unsigned>(row.routeState),
                      static_cast<unsigned>(row.freeR), static_cast<unsigned>(row.garbR),
                      static_cast<unsigned>(row.youngR), static_cast<unsigned>(row.ghostR),
                      static_cast<unsigned>(row.marked), static_cast<unsigned>(row.live0),
                      row.fwdWord, static_cast<unsigned>(row.holderYoung),
                      static_cast<unsigned>(row.classKind), row.holderObj, row.holderStart,
                      static_cast<unsigned>(row.holderMarked), static_cast<unsigned>(row.holderMb),
                      static_cast<unsigned>(row.holderFree), static_cast<unsigned>(row.holderGarb),
                      static_cast<unsigned>(row.holderAuth), static_cast<unsigned>(row.holderLive0),
                      static_cast<unsigned>(row.holderNeverExam),
                      static_cast<unsigned>(row.holderRtype),
                      static_cast<unsigned>(row.holderResolved),
                      static_cast<unsigned>(row.myBranch));
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
    // regionlife: last free of CAS-null target (same-life via lifeId anti-reuse).
    if (WhoZeroOn() && row.tgt != 0 && (row.freeR || row.garbR || row.classKind == 2)) {
        RegionLifeDiag::DumpJoinForTarget(row.tgt, tag);
    }
    if (EdgeMissOn() && row.myBranch == 2 && row.tgt != 0) {
        DumpEdgeJoin(row.slot, row.holderObj, row.tgt, tag);
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

    unsigned ghost = 0;
    unsigned route = 0;
    uint64_t liveBytes = 0;
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
            ghost = region->IsGhostFromRegion() ? 1 : 0;
            route = static_cast<unsigned>(region->GetRouteState());
            liveBytes = region->GetLiveByteCount();
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
                        "rstart=%#zx rend=%#zx alloc=%#zx collect=%u ghost=%u route=%u live=%llu\n",
                       tag, addr, inHeap, valid, allZero, nZero, nWords, tip, tipInHeap, tipAlign,
                       nameBuf[0] != '\0' ? nameBuf : "?", typeByte, static_cast<unsigned>(flag),
                       static_cast<unsigned>(fieldNum), instSz, header0, header1, rtype, young,
                       garbage, freeReg, inAlloc, rstart, rend, alloc, collectHits, ghost, route,
                       static_cast<unsigned long long>(liveBytes));
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

    if (inHeap == 1 && addr != 0) {
        char clearBuf[320];
        bool hit = TraceClear::Lookup(static_cast<MAddress>(addr), clearBuf, sizeof(clearBuf));
        char clearLine[400];
        int cn = sprintf_s(clearLine, sizeof(clearLine),
                           "[GCV2][threadzero] clear tag=%s hit=%u detail=%s\n",
                           tag, hit ? 1U : 0U, clearBuf[0] != '\0' ? clearBuf : "empty");
        if (cn > 0) {
            WriteLine(clearLine, static_cast<size_t>(cn));
        }
        if (rstart != 0 && rend > rstart) {
            uintptr_t headW[8];
            uintptr_t midW[8];
            uintptr_t tailW[8];
            std::memset(headW, 0, sizeof(headW));
            std::memset(midW, 0, sizeof(midW));
            std::memset(tailW, 0, sizeof(tailW));
            (void)CopyWords(rstart, headW, 8);
            uintptr_t mid = rstart + ((rend - rstart) / 2);
            (void)CopyWords(mid, midW, 8);
            uintptr_t tail = (alloc > rstart + 64) ? (alloc - 64) : rstart;
            if (tail + 64 > rend) {
                tail = rend > 64 ? (rend - 64) : rstart;
            }
            (void)CopyWords(tail, tailW, 8);
            unsigned headZ = 0;
            unsigned midZ = 0;
            unsigned tailZ = 0;
            for (unsigned i = 0; i < 8; ++i) {
                headZ += (headW[i] == 0) ? 1U : 0U;
                midZ += (midW[i] == 0) ? 1U : 0U;
                tailZ += (tailW[i] == 0) ? 1U : 0U;
            }
            char samp[640];
            int sn = sprintf_s(samp, sizeof(samp),
                               "[GCV2][threadzero] region tag=%s off=%#zx rsz=%#zx "
                               "headZ=%u/%u midZ=%u/%u tailZ=%u/%u "
                               "head0=%#zx mid0=%#zx tail0=%#zx\n",
                               tag, addr - rstart, rend - rstart,
                               headZ, 8U, midZ, 8U, tailZ, 8U,
                               headW[0], midW[0], tailW[0]);
            if (sn > 0) {
                WriteLine(samp, static_cast<size_t>(sn));
            }
        }
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

void NoteCopy(const void* fromAddr, const void* toAddr, size_t size, uint32_t done)
{
    // whozero/edgemiss need the copy ring to reconcile rings against post-move slot addrs.
    if (!GateOn() && !WhoZeroOn() && !EdgeMissOn()) {
        return;
    }
    if (GateOn()) {
        HealthOnce();
        EnsureAtexit();
    }
    uintptr_t from = reinterpret_cast<uintptr_t>(fromAddr);
    uintptr_t to = reinterpret_cast<uintptr_t>(toAddr);
    uint16_t phase = CurrentPhase();
    if (done == 0) {
        g_inflight.from = from;
        g_inflight.to = to;
        g_inflight.size = static_cast<uint32_t>(size);
        g_inflight.phase = phase;
        if (g_globalSlot < 0) {
            for (size_t i = 0; i < kGlobalInflightCap; ++i) {
                uint16_t expected = 0;
                if (g_globalInflight[i].live.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                    g_globalInflight[i].from.store(from, std::memory_order_relaxed);
                    g_globalInflight[i].to.store(to, std::memory_order_relaxed);
                    g_globalInflight[i].size.store(static_cast<uint32_t>(size), std::memory_order_relaxed);
                    g_globalInflight[i].phase.store(phase, std::memory_order_relaxed);
                    g_globalSlot = static_cast<int>(i);
                    break;
                }
            }
        }
    } else {
        g_inflight.from = 0;
        g_inflight.to = 0;
        g_inflight.size = 0;
        g_inflight.phase = 0;
        if (g_globalSlot >= 0) {
            GlobalInflight& slot = g_globalInflight[static_cast<size_t>(g_globalSlot)];
            slot.from.store(0, std::memory_order_relaxed);
            slot.to.store(0, std::memory_order_relaxed);
            slot.size.store(0, std::memory_order_relaxed);
            slot.phase.store(0, std::memory_order_relaxed);
            slot.live.store(0, std::memory_order_release);
            g_globalSlot = -1;
        }
    }
    uint32_t seq = static_cast<uint32_t>(g_copyTotal.fetch_add(1, std::memory_order_relaxed) + 1);
    uint32_t slotIdx = g_copyNext.fetch_add(1, std::memory_order_relaxed);
    if (slotIdx >= kCopyCap) {
        g_copyWrap.fetch_add(1, std::memory_order_relaxed);
    }
    CopyRow& row = g_copies[slotIdx % kCopyCap];
    row.from = from;
    row.to = to;
    row.size = static_cast<uint32_t>(size);
    row.phase = phase;
    row.done = static_cast<uint16_t>(done);
    row.seq = seq;
    // Move-independent whozero: rewrite zero-ring slots that lived in the from-object.
    // Page filter keeps this off the cold path for objects without prior null writes.
    if (done != 0 && WhoZeroOn() && size > 0 && from != 0 && to != 0 &&
        ZeroRangeMaybeMarked(from, static_cast<uintptr_t>(size))) {
        uint32_t zn = static_cast<uint32_t>(
            g_zeroTotal.load(std::memory_order_acquire) < kZeroCap ?
                g_zeroTotal.load(std::memory_order_relaxed) : kZeroCap);
        uint32_t znext = g_zeroNext.load(std::memory_order_acquire);
        size_t ztot = g_zeroTotal.load(std::memory_order_acquire);
        uint32_t zbase = (ztot < kZeroCap) ? 0 : (znext % kZeroCap);
        uintptr_t f0 = from;
        uintptr_t t0 = to;
        uintptr_t sz = static_cast<uintptr_t>(size);
        for (uint32_t zi = 0; zi < zn; ++zi) {
            ZeroRow& zr = g_zeros[(zbase + zi) % kZeroCap];
            uintptr_t s = zr.slot;
            if (s >= f0 && s < f0 + sz) {
                uintptr_t ns = t0 + (s - f0);
                zr.slot = ns;
                MarkZeroPage(ns);
                g_zeroRemap.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    // edgemiss: remap edge-ring slots + mark-ring objects that lived in from-object.
    if (done != 0 && EdgeMissOn() && size > 0 && from != 0 && to != 0) {
        uintptr_t f0 = from;
        uintptr_t t0 = to;
        uintptr_t sz = static_cast<uintptr_t>(size);
        if (EdgeRangeMaybeMarked(from, sz)) {
            uint32_t en = static_cast<uint32_t>(
                g_edgeTotal.load(std::memory_order_acquire) < kEdgeCap ?
                    g_edgeTotal.load(std::memory_order_relaxed) : kEdgeCap);
            uint32_t enext = g_edgeNext.load(std::memory_order_acquire);
            size_t etot = g_edgeTotal.load(std::memory_order_acquire);
            uint32_t ebase = (etot < kEdgeCap) ? 0 : (enext % kEdgeCap);
            for (uint32_t ei = 0; ei < en; ++ei) {
                EdgeRow& er = g_edges[(ebase + ei) % kEdgeCap];
                if (er.slot >= f0 && er.slot < f0 + sz) {
                    er.slot = t0 + (er.slot - f0);
                    MarkEdgePage(er.slot);
                    g_edgeRemap.fetch_add(1, std::memory_order_relaxed);
                }
                if (er.holder >= f0 && er.holder < f0 + sz) {
                    er.holder = t0 + (er.holder - f0);
                }
            }
        }
        if (EdgeRangeMaybeMarked(from, sz)) {
            uint32_t mn = static_cast<uint32_t>(
                g_markTotal.load(std::memory_order_acquire) < kMarkCap ?
                    g_markTotal.load(std::memory_order_relaxed) : kMarkCap);
            uint32_t mnext = g_markNext.load(std::memory_order_acquire);
            size_t mtot = g_markTotal.load(std::memory_order_acquire);
            uint32_t mbase = (mtot < kMarkCap) ? 0 : (mnext % kMarkCap);
            for (uint32_t mi = 0; mi < mn; ++mi) {
                MarkRow& mr = g_marks[(mbase + mi) % kMarkCap];
                if (mr.obj >= f0 && mr.obj < f0 + sz) {
                    mr.obj = t0 + (mr.obj - f0);
                    MarkEdgePage(mr.obj);
                    g_markRemap.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
}

uint64_t MidCopyStallNs()
{
    static const uint64_t ns = []() -> uint64_t {
        const char* v = std::getenv("MRT_GCV2_COPYSTALL_NS");
        if (v == nullptr || v[0] == '\0' || v[0] == '0') {
            return 0;
        }
        char* end = nullptr;
        unsigned long long parsed = std::strtoull(v, &end, 10);
        if (end == v || parsed == 0) {
            return 0;
        }
        return static_cast<uint64_t>(parsed);
    }();
    return ns;
}

void MaybeMidCopyStall(size_t size)
{
    uint64_t ns = MidCopyStallNs();
    if (ns == 0) {
        return;
    }
    static const size_t minSize = []() -> size_t {
        const char* v = std::getenv("MRT_GCV2_COPYSTALL_MIN");
        if (v == nullptr || v[0] == '\0') {
            return 224; // crash slots start at +0xe0
        }
        char* end = nullptr;
        unsigned long parsed = std::strtoul(v, &end, 10);
        if (end == v) {
            return 224;
        }
        return static_cast<size_t>(parsed);
    }();
    static const uint32_t maxStalls = []() -> uint32_t {
        const char* v = std::getenv("MRT_GCV2_COPYSTALL_MAX");
        if (v == nullptr || v[0] == '\0') {
            return 32;
        }
        char* end = nullptr;
        unsigned long parsed = std::strtoul(v, &end, 10);
        if (end == v) {
            return 32;
        }
        return static_cast<uint32_t>(parsed);
    }();
    static std::atomic<uint32_t> stalled{ 0 };
    if (size < minSize) {
        return;
    }
    if (stalled.fetch_add(1, std::memory_order_relaxed) >= maxStalls) {
        return;
    }
    TimeUtil::SleepForNano(ns);
}

void FillZeroTargetSnapshot(ZeroRow& row)
{
    row.tgt = 0;
    row.fwdWord = 0;
    row.rtype = 0xff;
    row.routeState = 0xff;
    row.freeR = 0;
    row.garbR = 0;
    row.youngR = 0xff;
    row.ghostR = 0;
    row.marked = 0xff;
    row.live0 = 0xff;
    row.holderYoung = 0xff;
    row.classKind = 0;
    row.holderObj = 0;
    row.holderStart = 0;
    row.holderMarked = 0xff;
    row.holderMb = 0xff;
    row.holderFree = 0;
    row.holderGarb = 0;
    row.holderAuth = 0xff;
    row.holderLive0 = 0xff;
    row.holderNeverExam = 0xff;
    row.myBranch = 0;
    row.holderRtype = 0xff;
    row.holderResolved = 0;
    if (!WhoZeroOn()) {
        return;
    }
    // Peel colour bits (low 48 on our heap pointers).
    uintptr_t tgt = row.oldRaw & 0x0000ffffffffffffULL;
    row.tgt = tgt;
    if (row.slot != 0 && Runtime::CurrentRef() != nullptr && Heap::IsHeapAddress(row.slot)) {
        RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(static_cast<MAddress>(row.slot));
        if (hr != nullptr) {
            row.holderYoung = hr->IsYoungRegion() ? 1 : 0;
            row.holderStart = hr->GetRegionStart();
            row.holderRtype = static_cast<uint8_t>(hr->GetRegionType());
            row.holderFree = hr->IsFreeRegion() ? 1 : 0;
            row.holderGarb = hr->IsGarbageRegion() ? 1 : 0;
            row.holderAuth = hr->IsLiveCountAuthoritative() ? 1 : 0;
            RegionBitmap* mb = hr->IsYoungRegion()
                ? hr->GetMarkBitmap(hr->GetMarkView<Generation::Young>())
                : hr->GetMarkBitmap(hr->GetMarkView<Generation::Old>());
            row.holderMb = (mb != nullptr) ? 1 : 0;
            const bool knownEmpty = hr->IsYoungRegion()
                ? hr->IsKnownYoungEmpty(hr->GetMarkView<Generation::Young>())
                : hr->IsKnownEmpty(hr->GetMarkView<Generation::Old>());
            row.holderNeverExam =
                (knownEmpty && mb == nullptr && hr->GetRegionAllocPtr() > hr->GetRegionStart()) ? 1 : 0;
            // Resolve holder object covering this slot (move-independent via region start above).
            BaseObject* host = nullptr;
            if (!hr->IsFreeRegion() && !hr->IsGarbageRegion()) {
                hr->VisitAllObjects([&host, slot = row.slot](BaseObject* holder) {
                    if (host != nullptr || holder == nullptr) {
                        return;
                    }
                    uintptr_t h = reinterpret_cast<uintptr_t>(holder);
                    if (slot < h) {
                        return;
                    }
                    if (holder->HasRefField()) {
                        holder->ForEachRefField([holder, &host, slot](RefField<>& field) {
                            if (reinterpret_cast<uintptr_t>(&field) == slot) {
                                host = holder;
                            }
                        });
                    }
                });
            }
            if (host != nullptr) {
                row.holderResolved = 1;
                row.holderObj = reinterpret_cast<uintptr_t>(host);
                if (Collector::PlausibleManagedObjectGate("majoryoung.holder", host)) {
                    row.holderMarked = hr->IsYoungRegion()
                        ? (hr->IsMarkedObject(hr->GetMarkView<Generation::Young>(), host) ? 1 : 0)
                        : (hr->IsMarkedObject(hr->GetMarkView<Generation::Old>(), host) ? 1 : 0);
                    size_t hoff = hr->GetAddressOffset(static_cast<MAddress>(row.holderObj));
                    LiveInfo* hg0 = hr->GetLiveInfo0ForProbe();
                    row.holderLive0 = (hg0 != nullptr && hr->IsRouteSurvivedObject(hoff)) ? 1 : 0;
                } else {
                    row.holderMarked = 0;
                    row.holderLive0 = 0;
                }
            } else {
                // Fall back: mark bit at field offset is wrong semantics; report region face only.
                row.holderMarked = 0xff;
            }
            // majoryoung branch (holder face at CAS-null).
            if (row.holderMarked == 1) {
                row.myBranch = 2; // 乙 edge miss
            } else if (row.holderMarked == 0) {
                if (row.holderFree || row.holderGarb) {
                    row.myBranch = 1; // 甲 holder also dead
                } else {
                    row.myBranch = 3; // 丙 holder unmarked but region kept
                }
            }
        }
    }
    if (tgt == 0 || Runtime::CurrentRef() == nullptr || !Heap::IsHeapAddress(tgt)) {
        return;
    }
    RegionInfo* reg = RegionInfo::GetGhostFromRegionAt(static_cast<MAddress>(tgt));
    if (reg == nullptr) {
        reg = RegionInfo::TryGetRegionInfoAt(static_cast<MAddress>(tgt));
    }
    if (reg == nullptr) {
        row.classKind = 4; // 丁: no region / not in cset view
        return;
    }
    row.rtype = static_cast<uint8_t>(reg->GetRegionType());
    row.routeState = static_cast<uint8_t>(reg->GetRouteState());
    row.freeR = reg->IsFreeRegion() ? 1 : 0;
    row.garbR = reg->IsGarbageRegion() ? 1 : 0;
    row.youngR = reg->IsYoungRegion() ? 1 : 0;
    row.ghostR = reg->IsGhostFromRegion() ? 1 : 0;
    size_t off = reg->GetAddressOffset(static_cast<MAddress>(tgt));
    LiveInfo* g0 = reg->GetLiveInfo0ForProbe();
    row.live0 = (g0 != nullptr && reg->IsRouteSurvivedObject(off)) ? 1 : 0;
    BaseObject* obj = reinterpret_cast<BaseObject*>(tgt);
    if (Collector::PlausibleManagedObjectGate("noforward.zero", obj)) {
        row.marked = reg->IsRouteMarkedObject(obj) ? 1 : 0;
        row.fwdWord = static_cast<uint32_t>(obj->GetStateWord().GetObjectState().GetStateBits());
    } else {
        row.marked = 0;
    }
    // Classify why "no to-version" / null install (executor view).
    if (row.freeR || row.garbR) {
        row.classKind = 2; // 乙 recycled region
    } else if (row.ghostR == 0 && row.site == static_cast<uint16_t>(HealSite::WCollectorMinorFixForwardNull)) {
        row.classKind = 4; // 丁: fix moved a non-ghost / non-cset target
    } else if (row.marked == 0 || row.live0 == 0) {
        row.classKind = 1; // 甲 not marked / not in liveInfo0 ⇒ not evacuated
    } else {
        row.classKind = 3; // 丙 marked+live0 but still no to-version
    }
}

void NoteEdgeWrite(const void* holder, const void* slot, uintptr_t oldRaw, uintptr_t newRaw, uint8_t barrierKind)
{
    if (!EdgeMissOn()) {
        return;
    }
    uintptr_t tgt = newRaw & 0x0000ffffffffffffULL;
    uintptr_t oldTgt = oldRaw & 0x0000ffffffffffffULL;
    // Narrow: only null→large installs (edge establish). Avoid ring thrash on retargets.
    if (oldTgt != 0 || tgt == 0 || Runtime::CurrentRef() == nullptr || !Heap::IsHeapAddress(tgt)) {
        return;
    }
    RegionInfo* reg = RegionInfo::TryGetRegionInfoAt(static_cast<MAddress>(tgt));
    if (reg == nullptr || !reg->IsLargeRegion()) {
        return;
    }
    uint32_t seq = static_cast<uint32_t>(g_edgeTotal.fetch_add(1, std::memory_order_relaxed) + 1);
    uint32_t slotIdx = g_edgeNext.fetch_add(1, std::memory_order_relaxed);
    if (slotIdx >= kEdgeCap) {
        g_edgeWrap.fetch_add(1, std::memory_order_relaxed);
    }
    EdgeRow& row = g_edges[slotIdx % kEdgeCap];
    row.slot = reinterpret_cast<uintptr_t>(slot);
    row.oldRaw = oldRaw;
    row.newRaw = newRaw;
    row.tgt = tgt;
    row.holder = reinterpret_cast<uintptr_t>(holder);
    row.pc = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    row.gcCount = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    row.seq = seq;
    row.phase = CurrentPhase();
    row.barrier = barrierKind;
    const bool satb = PhaseSatbActive(row.phase);
    row.satbOld = (satb && ((oldRaw & 0x0000ffffffffffffULL) != 0)) ? 1 : 0;
    row.satbNew = (satb && tgt != 0) ? 1 : 0;
    row.large = 1;
    row.holderMarked = 0xff;
    row.holderYoung = 0xff;
    if (holder != nullptr && Heap::IsHeapAddress(holder)) {
        RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(static_cast<MAddress>(row.holder));
        if (hr != nullptr) {
            row.holderYoung = hr->IsYoungRegion() ? 1 : 0;
            BaseObject* hobj = reinterpret_cast<BaseObject*>(const_cast<void*>(holder));
            if (Collector::PlausibleManagedObjectGate("edgemiss.holder", hobj)) {
                row.holderMarked = hr->IsMarkedObject(hobj) ? 1 : 0;
            }
        }
    }
    MarkEdgePage(row.slot);
    if (seq <= 64) {
        char line[512];
        int n = sprintf_s(line, sizeof(line),
                          "[GCV2][edgemiss] write n=%u slot=%#zx old=%#zx new=%#zx tgt=%#zx "
                          "holder=%#zx phase=%u gc=%u barrier=%s(%u) satbOld=%u satbNew=%u "
                          "hMarked=%u hYoung=%u pc=%#zx\n",
                          seq, row.slot, static_cast<size_t>(oldRaw), static_cast<size_t>(newRaw), tgt,
                          row.holder, static_cast<unsigned>(row.phase), row.gcCount, BarrierName(row.barrier),
                          static_cast<unsigned>(row.barrier), static_cast<unsigned>(row.satbOld),
                          static_cast<unsigned>(row.satbNew), static_cast<unsigned>(row.holderMarked),
                          static_cast<unsigned>(row.holderYoung), row.pc);
        if (n > 0) {
            WriteLine(line, static_cast<size_t>(n));
        }
    }
}

void NoteFirstMark(const void* obj)
{
    if (!EdgeMissOn() || obj == nullptr) {
        return;
    }
    // Narrow: only young holders matter for majoryoung join (avoid mark-ring thrash).
    uintptr_t o = reinterpret_cast<uintptr_t>(obj);
    if (Runtime::CurrentRef() == nullptr || !Heap::IsHeapAddress(o)) {
        return;
    }
    RegionInfo* reg = RegionInfo::TryGetRegionInfoAt(static_cast<MAddress>(o));
    if (reg == nullptr || !reg->IsYoungRegion()) {
        return;
    }
    uint32_t seq = static_cast<uint32_t>(g_markTotal.fetch_add(1, std::memory_order_relaxed) + 1);
    uint32_t slotIdx = g_markNext.fetch_add(1, std::memory_order_relaxed);
    if (slotIdx >= kMarkCap) {
        g_markWrap.fetch_add(1, std::memory_order_relaxed);
    }
    MarkRow& row = g_marks[slotIdx % kMarkCap];
    row.obj = o;
    row.start = reg->GetRegionStart();
    row.off = static_cast<uint32_t>(reg->GetAddressOffset(static_cast<MAddress>(o)));
    row.gcCount = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    row.seq = seq;
    row.phase = CurrentPhase();
    MarkEdgePage(o); // reuse page filter so copy remap only scans when needed
}

void NoteZeroWrite(const void* slot, uintptr_t oldRaw, uintptr_t newRaw, uint16_t site)
{
    (void)newRaw;
    if (!ZeroTrackOn()) {
        return;
    }
    if (GateOn()) {
        HealthOnce();
        EnsureAtexit();
    }
    uint32_t seq = static_cast<uint32_t>(g_zeroTotal.fetch_add(1, std::memory_order_relaxed) + 1);
    uint32_t slotIdx = g_zeroNext.fetch_add(1, std::memory_order_relaxed);
    if (slotIdx >= kZeroCap) {
        g_zeroWrap.fetch_add(1, std::memory_order_relaxed);
    }
    ZeroRow& row = g_zeros[slotIdx % kZeroCap];
    row.slot = reinterpret_cast<uintptr_t>(slot);
    row.oldRaw = oldRaw;
    row.site = site;
    row.phase = CurrentPhase();
    row.seq = seq;
    FillZeroTargetSnapshot(row);
    if (WhoZeroOn()) {
        MarkZeroPage(row.slot);
    }
    // whozero: rare-path log only (successful CAS to null). Cap keeps stderr bounded.
    if (WhoZeroOn() && seq <= 256) {
        char line[640];
        int n = sprintf_s(line, sizeof(line),
                          "[GCV2][whozero] path=heal_null n=%u slot=%p old=%#zx site=%s(%u) "
                          "phase=%u tgt=%#zx rtype=%u rs=%u free=%u garb=%u young=%u ghost=%u "
                          "marked=%u live0=%u fwd=%#x holderYoung=%u class=%u "
                          "hObj=%#zx hStart=%#zx hMarked=%u hMb=%u hFree=%u hGarb=%u "
                          "hAuth=%u hLive0=%u hNeverExam=%u myBranch=%u\n",
                          seq, slot, static_cast<size_t>(oldRaw), SiteName(site),
                          static_cast<unsigned>(site), static_cast<unsigned>(row.phase), row.tgt,
                          static_cast<unsigned>(row.rtype), static_cast<unsigned>(row.routeState),
                          static_cast<unsigned>(row.freeR), static_cast<unsigned>(row.garbR),
                          static_cast<unsigned>(row.youngR), static_cast<unsigned>(row.ghostR),
                          static_cast<unsigned>(row.marked), static_cast<unsigned>(row.live0),
                          row.fwdWord, static_cast<unsigned>(row.holderYoung),
                          static_cast<unsigned>(row.classKind), row.holderObj, row.holderStart,
                          static_cast<unsigned>(row.holderMarked), static_cast<unsigned>(row.holderMb),
                          static_cast<unsigned>(row.holderFree), static_cast<unsigned>(row.holderGarb),
                          static_cast<unsigned>(row.holderAuth), static_cast<unsigned>(row.holderLive0),
                          static_cast<unsigned>(row.holderNeverExam),
                          static_cast<unsigned>(row.myBranch));
        if (n > 0) {
            WriteLine(line, static_cast<size_t>(n));
        }
        if (row.tgt != 0 && (row.freeR || row.garbR || row.classKind == 2)) {
            RegionLifeDiag::DumpJoinForTarget(row.tgt, "heal_null");
        }
        if (EdgeMissOn() && row.myBranch == 2 && row.tgt != 0) {
            DumpEdgeJoin(row.slot, row.holderObj, row.tgt, "heal_null");
        }
    }
}

void NoteCrashWhoZero(uintptr_t r13, uintptr_t rcx, uintptr_t rsi, uintptr_t rbx, uintptr_t r12)
{
    if (!ZeroTrackOn()) {
        return;
    }
    // Signature A (LexerImpl::Scan): r13=holder, bytes@+0x28, end@+0x38, cursor@+0x48; rcx=Array* after peel.
    uintptr_t slotBytes = (r13 != 0) ? (r13 + 0x28U) : 0;
    uintptr_t rawBytes = 0;
    uintptr_t endVal = 0;
    uintptr_t cursorVal = 0;
    uintptr_t tip = 0;
    uintptr_t f30 = 0;
    unsigned holderInHeap = 0;
    unsigned slotOk = 0;
    if (r13 != 0 && Runtime::CurrentRef() != nullptr && Heap::IsHeapAddress(r13)) {
        holderInHeap = 1;
        uintptr_t words[12];
        std::memset(words, 0, sizeof(words));
        if (CopyWords(r13, words, 12)) {
            slotOk = 1;
            tip = words[0] & 0xffffffffffffULL;
            f30 = words[6]; // +0x30
            rawBytes = words[5]; // +0x28
            endVal = words[7]; // +0x38
            cursorVal = words[9]; // +0x48
        }
    } else if (slotBytes != 0) {
        uintptr_t w[1] = { 0 };
        if (CopyWords(slotBytes, w, 1)) {
            slotOk = 1;
            rawBytes = w[0];
        }
    }
    const char* q1 = "unknown";
    if (slotOk) {
        if (rawBytes == 0) {
            q1 = "slot_was_0";
        } else if (rcx == 0) {
            q1 = "barrier_returned_0";
        } else {
            q1 = "slot_nonzero_rcx_nonzero";
        }
    }

    uint32_t zeroN = static_cast<uint32_t>(
        g_zeroTotal.load(std::memory_order_acquire) < kZeroCap ?
            g_zeroTotal.load(std::memory_order_relaxed) : kZeroCap);
    uint32_t zeroNext = g_zeroNext.load(std::memory_order_acquire);
    size_t zeroTotal = g_zeroTotal.load(std::memory_order_acquire);
    uint32_t zeroBase = (zeroTotal < kZeroCap) ? 0 : (zeroNext % kZeroCap);

    // Move-independent identity (口径丙+甲 hybrid):
    // Collect every historical address of the crash-time slot/holder via the copy ring
    // (from↔to remaps). Slot addresses that only differ by object relocation still hit.
    constexpr size_t kHistCap = 64;
    uintptr_t histSlot[kHistCap];
    size_t histN = 0;
    auto pushHist = [&](uintptr_t a) {
        if (a == 0 || histN >= kHistCap) {
            return;
        }
        for (size_t i = 0; i < histN; ++i) {
            if (histSlot[i] == a) {
                return;
            }
        }
        histSlot[histN++] = a;
    };
    pushHist(slotBytes);
    if (r13 != 0) {
        pushHist(r13 + 0x28U);
    }
    uint32_t copyN = static_cast<uint32_t>(
        g_copyTotal.load(std::memory_order_acquire) < kCopyCap ?
            g_copyTotal.load(std::memory_order_relaxed) : kCopyCap);
    uint32_t copyNext = g_copyNext.load(std::memory_order_acquire);
    size_t copyTotal = g_copyTotal.load(std::memory_order_acquire);
    uint32_t copyBase = (copyTotal < kCopyCap) ? 0 : (copyNext % kCopyCap);
    // Iterate remaps: if a hist address lies in a completed copy's from/to range, add the twin.
    for (unsigned round = 0; round < 8; ++round) {
        size_t before = histN;
        for (uint32_t ci = 0; ci < copyN; ++ci) {
            const CopyRow& cr = g_copies[(copyBase + ci) % kCopyCap];
            if (cr.done == 0 || cr.size == 0 || cr.from == 0 || cr.to == 0) {
                continue;
            }
            uintptr_t f0 = cr.from;
            uintptr_t t0 = cr.to;
            uintptr_t sz = cr.size;
            size_t nNow = histN;
            for (size_t hi = 0; hi < nNow; ++hi) {
                uintptr_t a = histSlot[hi];
                if (a >= f0 && a < f0 + sz) {
                    pushHist(t0 + (a - f0));
                }
                if (a >= t0 && a < t0 + sz) {
                    pushHist(f0 + (a - t0));
                }
            }
        }
        if (histN == before) {
            break;
        }
    }

    uint32_t exactHits = 0;
    uint32_t moveHits = 0;
    uint32_t lastIdx = 0;
    uint32_t lastMoveIdx = 0;
    bool haveExact = false;
    bool haveMove = false;
    uint32_t hitSiteCounts[8] = {};
    const uint16_t trackSites[] = {
        static_cast<uint16_t>(HealSite::WCollectorMinorFixForwardNull),
        static_cast<uint16_t>(HealSite::WCollectorMinorResolveDead),
        static_cast<uint16_t>(HealSite::WCollectorFixOldTaggedDead),
        static_cast<uint16_t>(HealSite::WCollectorRemsetResolveDead),
        static_cast<uint16_t>(HealSite::WCollectorResolveDeadRoot),
        static_cast<uint16_t>(HealSite::BaseObjectCompareExchangeRefField),
        static_cast<uint16_t>(HealSite::PreforwardReadReference),
        static_cast<uint16_t>(HealSite::ForwardReadReference),
    };
    for (uint32_t i = 0; i < zeroN; ++i) {
        uint32_t idx = (zeroBase + i) % kZeroCap;
        const ZeroRow& row = g_zeros[idx];
        if (slotBytes != 0 && row.slot == slotBytes) {
            ++exactHits;
            lastIdx = idx;
            haveExact = true;
        } else if (row.slot != 0) {
            for (size_t hi = 0; hi < histN; ++hi) {
                if (row.slot == histSlot[hi]) {
                    ++moveHits;
                    lastMoveIdx = idx;
                    haveMove = true;
                    break;
                }
            }
        }
        for (size_t s = 0; s < 8; ++s) {
            if (row.site == trackSites[s]) {
                ++hitSiteCounts[s];
            }
        }
    }
    uint32_t anyHits = exactHits + moveHits;

    char line[896];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][whozero] crash q1=%s holder=%#zx holderInHeap=%u slotBytes=%#zx "
                      "rawBytes=%#zx end=%#zx cursor=%#zx f30=%#zx tip=%#zx rcx=%#zx rsi=%#zx "
                      "rbx=%#zx r12=%#zx zeroTotal=%zu zeroWrap=%zu exactHits=%u moveHits=%u "
                      "anyHits=%u histN=%zu copyTotal=%zu zeroRemap=%zu "
                      "siteFixNull=%u siteResolve=%u siteF3=%u siteRemset=%u siteRoot=%u "
                      "siteBaseCE=%u sitePreR=%u siteFwdR=%u\n",
                      q1, r13, holderInHeap, slotBytes, rawBytes, endVal, cursorVal, f30, tip,
                      rcx, rsi, rbx, r12, zeroTotal, g_zeroWrap.load(std::memory_order_relaxed),
                      exactHits, moveHits, anyHits, histN, copyTotal,
                      g_zeroRemap.load(std::memory_order_relaxed), hitSiteCounts[0],
                      hitSiteCounts[1], hitSiteCounts[2], hitSiteCounts[3], hitSiteCounts[4],
                      hitSiteCounts[5], hitSiteCounts[6], hitSiteCounts[7]);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
    if (haveExact) {
        DumpZero("whozeroExact", g_zeros[lastIdx]);
        // gatedrop: join CAS-null target against TraceRefField reject ring.
        if (GateDropDiag::Enabled()) {
            GateDropDiag::NoteCrashJoin(r13, slotBytes, g_zeros[lastIdx].oldRaw);
        }
        // notraced: was TraceObjectRefFields called for this holder?
        if (NoTracedDiag::Enabled()) {
            NoTracedDiag::NoteCrashJoin(r13, g_zeros[lastIdx].holderObj);
        }
    } else if (haveMove) {
        DumpZero("whozeroMove", g_zeros[lastMoveIdx]);
        if (GateDropDiag::Enabled()) {
            GateDropDiag::NoteCrashJoin(r13, slotBytes, g_zeros[lastMoveIdx].oldRaw);
        }
        if (NoTracedDiag::Enabled()) {
            NoTracedDiag::NoteCrashJoin(r13, g_zeros[lastMoveIdx].holderObj);
        }
    } else if (slotBytes != 0) {
        char miss[256];
        int mn = sprintf_s(miss, sizeof(miss),
                           "[GCV2][whozero] exact_miss slot=%#zx histN=%zu copyTotal=%zu "
                           "(not in HealSlot-null ring under move-aware match)\n",
                           slotBytes, histN, copyTotal);
        if (mn > 0) {
            WriteLine(miss, static_cast<size_t>(mn));
        }
        // Still report reject-ring emptiness even without a whozero tgt.
        if (GateDropDiag::Enabled()) {
            GateDropDiag::NoteCrashJoin(r13, slotBytes, 0);
        }
        if (NoTracedDiag::Enabled()) {
            NoTracedDiag::NoteCrashJoin(r13, 0);
        }
    }
    // Crash-time holder mark face (r13 = LexerImpl holder). Complements CAS-null snapshot.
    if (holderInHeap && r13 != 0) {
        unsigned crashHMarked = 0xff;
        unsigned crashHMb = 0xff;
        unsigned crashHYoung = 0xff;
        unsigned crashHFree = 0;
        unsigned crashHGarb = 0;
        unsigned crashHAuth = 0xff;
        unsigned crashHNever = 0xff;
        uintptr_t crashHStart = 0;
        RegionInfo* chr = RegionInfo::TryGetRegionInfoAt(static_cast<MAddress>(r13));
        if (chr != nullptr) {
            crashHStart = chr->GetRegionStart();
            crashHYoung = chr->IsYoungRegion() ? 1 : 0;
            crashHFree = chr->IsFreeRegion() ? 1 : 0;
            crashHGarb = chr->IsGarbageRegion() ? 1 : 0;
            crashHAuth = chr->IsLiveCountAuthoritative() ? 1 : 0;
            RegionBitmap* cmb = chr->IsYoungRegion()
                ? chr->GetMarkBitmap(chr->GetMarkView<Generation::Young>())
                : chr->GetMarkBitmap(chr->GetMarkView<Generation::Old>());
            crashHMb = (cmb != nullptr) ? 1 : 0;
            const bool cke = chr->IsYoungRegion()
                ? chr->IsKnownYoungEmpty(chr->GetMarkView<Generation::Young>())
                : chr->IsKnownEmpty(chr->GetMarkView<Generation::Old>());
            crashHNever =
                (cke && cmb == nullptr && chr->GetRegionAllocPtr() > chr->GetRegionStart()) ? 1 : 0;
            BaseObject* hobj = reinterpret_cast<BaseObject*>(r13);
            if (Collector::PlausibleManagedObjectGate("majoryoung.crash_holder", hobj)) {
                crashHMarked = chr->IsYoungRegion()
                    ? (chr->IsMarkedObject(chr->GetMarkView<Generation::Young>(), hobj) ? 1 : 0)
                    : (chr->IsMarkedObject(chr->GetMarkView<Generation::Old>(), hobj) ? 1 : 0);
            } else {
                crashHMarked = 0;
            }
        }
        char hline[384];
        int hn = sprintf_s(hline, sizeof(hline),
                           "[GCV2][majoryoung] crash_holder=%#zx hStart=%#zx hYoung=%u "
                           "hMarked=%u hMb=%u hFree=%u hGarb=%u hAuth=%u hNeverExam=%u "
                           "(CAS-null myBranch on whozeroExact; crash face may be later cycle)\n",
                           r13, crashHStart, crashHYoung, crashHMarked, crashHMb, crashHFree,
                           crashHGarb, crashHAuth, crashHNever);
        if (hn > 0) {
            WriteLine(hline, static_cast<size_t>(hn));
        }
    }
    // Not whole-object ClearUnits: end/cursor should stay small non-zero Int64s if only bytes died.
    unsigned fieldsAlive = (endVal != 0 || cursorVal != 0) ? 1U : 0U;
    char clearLine[192];
    int cn = sprintf_s(clearLine, sizeof(clearLine),
                       "[GCV2][whozero] clearunits_whole_object=%u (0=other_fields_nonzero)\n",
                       fieldsAlive == 0 && holderInHeap ? 1U : 0U);
    if (cn > 0) {
        WriteLine(clearLine, static_cast<size_t>(cn));
    }
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

void JoinCopyAndZero(uintptr_t holder)
{
    uint32_t copyN = static_cast<uint32_t>(
        g_copyTotal.load(std::memory_order_acquire) < kCopyCap ?
            g_copyTotal.load(std::memory_order_relaxed) : kCopyCap);
    uint32_t copyNext = g_copyNext.load(std::memory_order_acquire);
    size_t copyTotal = g_copyTotal.load(std::memory_order_acquire);
    uint32_t copyBase = (copyTotal < kCopyCap) ? 0 : (copyNext % kCopyCap);
    uint32_t holderAsTo = 0;
    uint32_t holderAsFrom = 0;
    uint32_t lastToIdx = 0;
    uint32_t lastFromIdx = 0;
    bool haveTo = false;
    bool haveFrom = false;
    for (uint32_t i = 0; i < copyN; ++i) {
        uint32_t idx = (copyBase + i) % kCopyCap;
        const CopyRow& row = g_copies[idx];
        if (holder != 0 && holder >= row.to && holder < row.to + row.size) {
            ++holderAsTo;
            lastToIdx = idx;
            haveTo = true;
        }
        if (holder != 0 && holder >= row.from && holder < row.from + row.size) {
            ++holderAsFrom;
            lastFromIdx = idx;
            haveFrom = true;
        }
    }

    uint32_t zeroN = static_cast<uint32_t>(
        g_zeroTotal.load(std::memory_order_acquire) < kZeroCap ?
            g_zeroTotal.load(std::memory_order_relaxed) : kZeroCap);
    uint32_t zeroNext = g_zeroNext.load(std::memory_order_acquire);
    size_t zeroTotal = g_zeroTotal.load(std::memory_order_acquire);
    uint32_t zeroBase = (zeroTotal < kZeroCap) ? 0 : (zeroNext % kZeroCap);
    uint32_t slotHits = 0;
    uint32_t lastZeroIdx = 0;
    bool haveZero = false;
    uintptr_t holderEnd = holder != 0 ? (holder + 512U) : 0;
    for (uint32_t i = 0; i < zeroN; ++i) {
        uint32_t idx = (zeroBase + i) % kZeroCap;
        const ZeroRow& row = g_zeros[idx];
        if (holder != 0 && row.slot >= holder && row.slot < holderEnd) {
            ++slotHits;
            lastZeroIdx = idx;
            haveZero = true;
        }
    }

    uint32_t globalLive = 0;
    uint32_t holderGlobal = 0;
    uint32_t lastGlobalIdx = 0;
    bool haveGlobal = false;
    for (size_t i = 0; i < kGlobalInflightCap; ++i) {
        if (g_globalInflight[i].live.load(std::memory_order_acquire) == 0) {
            continue;
        }
        uintptr_t gTo = g_globalInflight[i].to.load(std::memory_order_relaxed);
        uint32_t gSize = g_globalInflight[i].size.load(std::memory_order_relaxed);
        ++globalLive;
        if (holder != 0 && gTo != 0 && holder >= gTo && holder < gTo + gSize) {
            ++holderGlobal;
            lastGlobalIdx = static_cast<uint32_t>(i);
            haveGlobal = true;
        }
    }

    char line[640];
    int wn = sprintf_s(line, sizeof(line),
                       "[GCV2][slotwindow] join holder=%#zx copyTotal=%zu copyWrap=%zu "
                       "zeroTotal=%zu zeroWrap=%zu holderAsTo=%u holderAsFrom=%u slotZeroHits=%u "
                       "inflightFrom=%#zx inflightTo=%#zx inflightSize=%u inflightPhase=%u "
                       "inflightGlobal=%u inflightHolder=%u\n",
                       holder, copyTotal, g_copyWrap.load(std::memory_order_relaxed),
                       zeroTotal, g_zeroWrap.load(std::memory_order_relaxed),
                       holderAsTo, holderAsFrom, slotHits,
                       g_inflight.from, g_inflight.to, g_inflight.size,
                       static_cast<unsigned>(g_inflight.phase), globalLive, holderGlobal);
    if (wn > 0) {
        WriteLine(line, static_cast<size_t>(wn));
    }
    if (g_inflight.to != 0) {
        CopyRow inflight{};
        inflight.from = g_inflight.from;
        inflight.to = g_inflight.to;
        inflight.size = g_inflight.size;
        inflight.phase = g_inflight.phase;
        inflight.done = 0;
        inflight.seq = 0;
        DumpCopy("inflight", inflight);
    }
    if (haveGlobal) {
        CopyRow grow{};
        grow.from = g_globalInflight[lastGlobalIdx].from.load(std::memory_order_relaxed);
        grow.to = g_globalInflight[lastGlobalIdx].to.load(std::memory_order_relaxed);
        grow.size = g_globalInflight[lastGlobalIdx].size.load(std::memory_order_relaxed);
        grow.phase = g_globalInflight[lastGlobalIdx].phase.load(std::memory_order_relaxed);
        grow.done = 0;
        grow.seq = 0;
        DumpCopy("globalInflight", grow);
    }
    if (haveTo) {
        DumpCopy("holderTo", g_copies[lastToIdx]);
        DumpHolder("fromCopy", g_copies[lastToIdx].from);
    }
    if (haveFrom) {
        DumpCopy("holderFrom", g_copies[lastFromIdx]);
    }
    if (haveZero) {
        DumpZero("slotZero", g_zeros[lastZeroIdx]);
    }
    uint32_t tail = copyN < 4 ? copyN : 4;
    for (uint32_t i = 0; i < tail; ++i) {
        uint32_t idx = (copyBase + copyN - tail + i) % kCopyCap;
        DumpCopy("copyTail", g_copies[idx]);
    }
}

void NoteCrashRegs(uintptr_t rdi, uintptr_t rax, uintptr_t r12, uintptr_t r14, uintptr_t rbp)
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

    uintptr_t threadRaw = 0;
    uintptr_t lwt = 0;
    void* arg = CJ_CJThreadGetArg();
    if (arg != nullptr) {
        lwt = reinterpret_cast<uintptr_t>(arg);
        threadRaw = reinterpret_cast<uintptr_t>(static_cast<LWTData*>(arg)->threadObject);
    }
    uintptr_t threadPeeled = threadRaw & 0xffffffffffffULL;
    unsigned eqRaw = (rdi != 0 && rdi == threadRaw) ? 1U : 0U;
    unsigned eqPeeled = (rdi != 0 && rdi == threadPeeled) ? 1U : 0U;
    char thrLine[320];
    int tn = sprintf_s(thrLine, sizeof(thrLine),
                       "[GCV2][threadzero] currentThrRaw=%#zx peeled=%#zx rdi=%#zx "
                       "eqRaw=%u eqPeeled=%u\n",
                       threadRaw, threadPeeled, rdi, eqRaw, eqPeeled);
    if (tn > 0) {
        WriteLine(thrLine, static_cast<size_t>(tn));
    }
    if (threadPeeled != 0 && threadPeeled != rdi) {
        DumpHolder("curThr", threadPeeled);
    }

    uintptr_t slotAddr = (lwt != 0) ? (lwt + offsetof(LWTData, threadObject)) : 0;
    uintptr_t slotRaw = threadRaw;
    unsigned slotGhost = 0;
    unsigned slotInHeap = 0;
    unsigned fromGhost = 0;
    unsigned fromRoute = 255;
    unsigned fromLive0 = 0;
    uintptr_t fromStart = 0;
    uintptr_t fromEnd = 0;
    uintptr_t planTo1 = 0;
    uint32_t planTo1Used = 0;
    uint32_t planTo2Idx = 0;
    uintptr_t routed = 0;
    unsigned routedEqRdi = 0;
    unsigned latestEqRdi = 0;
    unsigned keepFrom = 0;
    uintptr_t retAddr = 0;
    if (rbp > 8) {
        uintptr_t words[1] = { 0 };
        if (CopyWords(rbp + 8, words, 1)) {
            retAddr = words[0];
        }
    }
    if (threadPeeled != 0 && Heap::IsHeapAddress(threadPeeled)) {
        slotInHeap = 1;
        BaseObject* fromObj = reinterpret_cast<BaseObject*>(threadPeeled);
        slotGhost = RegionInfo::InGhostFromRegion(fromObj) ? 1U : 0U;
        RegionInfo* ghostReg = RegionInfo::GetGhostFromRegionAt(threadPeeled);
        if (ghostReg != nullptr) {
            fromGhost = 1;
            fromRoute = static_cast<unsigned>(ghostReg->GetRouteState());
            fromStart = ghostReg->GetRegionStart();
            fromEnd = ghostReg->GetRegionEnd();
            fromLive0 = ghostReg->GetLiveInfo0ForProbe() != nullptr ? 1U : 0U;
            RouteInfo plan = ghostReg->GetRouteInfoForProbe();
            planTo1 = plan.toRegion1StartAddress;
            planTo1Used = plan.GetToRegion1UsedBytes();
            planTo2Idx = plan.GetToRegion2Idx();
            BaseObject* to = ghostReg->GetRouteForProbe(fromObj);
            if (to != nullptr) {
                routed = reinterpret_cast<uintptr_t>(to);
                routedEqRdi = (routed == rdi) ? 1U : 0U;
            }
        }
        if (Runtime::CurrentRef() != nullptr) {
            BaseObject* latest = Heap::GetHeap().GetCollector().FindLatestVersion(fromObj);
            if (latest != nullptr) {
                uintptr_t latestAddr = reinterpret_cast<uintptr_t>(latest);
                latestEqRdi = (latestAddr == rdi) ? 1U : 0U;
                keepFrom = (latestAddr == threadPeeled) ? 1U : 0U;
            }
        }
    }
    char srcLine[640];
    int sn = sprintf_s(srcLine, sizeof(srcLine),
                       "[GCV2][staleref] rdi=%#zx slot=%#zx slotRaw=%#zx slotInHeap=%u slotGhost=%u "
                       "fromGhost=%u fromRoute=%u fromLive0=%u from=[%#zx,%#zx) "
                       "planTo1=%#zx planTo1Used=%u planTo2Idx=%u routed=%#zx routedEqRdi=%u "
                       "latestEqRdi=%u keepFrom=%u ret=%#zx rbp=%#zx\n",
                       rdi, slotAddr, slotRaw, slotInHeap, slotGhost,
                       fromGhost, fromRoute, fromLive0, fromStart, fromEnd,
                       planTo1, planTo1Used, planTo2Idx, routed, routedEqRdi,
                       latestEqRdi, keepFrom, retAddr, rbp);
    if (sn > 0) {
        WriteLine(srcLine, static_cast<size_t>(sn));
    }
    if (routed != 0 && routed != rdi && routed != threadPeeled) {
        DumpHolder("routed", routed);
    }

    if (rdi >= 64 && Heap::IsHeapAddress(rdi - 64)) {
        DumpHolder("rdiM64", rdi - 64);
    }

    JoinCopyAndZero(r14);
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][healpair] report point=%s pairTotal=%zu pairWrap=%zu collectTotal=%zu "
        "collectWrap=%zu siteFwdInt=%zu sitePresInt=%zu siteFwdGhost=%zu siteNorm=%zu "
        "copyTotal=%zu zeroTotal=%zu env=MRT_GCV2_HEALPAIR=1",
        point != nullptr ? point : "none", g_pairTotal.load(std::memory_order_relaxed),
        g_pairWrap.load(std::memory_order_relaxed), g_collectTotal.load(std::memory_order_relaxed),
        g_collectWrap.load(std::memory_order_relaxed), g_bySite[0].load(std::memory_order_relaxed),
        g_bySite[1].load(std::memory_order_relaxed), g_bySite[2].load(std::memory_order_relaxed),
        g_bySite[3].load(std::memory_order_relaxed),
        g_copyTotal.load(std::memory_order_relaxed),
        g_zeroTotal.load(std::memory_order_relaxed));
}

} // namespace HealPairDiag
} // namespace MapleRuntime
