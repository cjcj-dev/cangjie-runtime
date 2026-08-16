// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/MarkFaceSnap.h"

#include "Heap/Verify/RegionLifeDiag.h"  // PATH_PRE_RELEASE_DECISION

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unistd.h>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Common/Runtime.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/HealPairDiag.h"
#include "securec.h"

namespace MapleRuntime {
namespace MarkFaceSnap {
namespace {

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

size_t EnvSize(const char* name, size_t dflt)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return dflt;
    }
    char* end = nullptr;
    unsigned long long v = std::strtoull(value, &end, 0);
    if (end == value || v == 0) {
        return dflt;
    }
    return static_cast<size_t>(v);
}

bool GateOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        return EnvIsOne("MRT_GCV2_HOLDERCAP") || DiagGate::TokenOn("holdercap");
    }();
    return on;
}

void WriteLine(const char* buf, size_t len)
{
    if (buf != nullptr && len > 0) {
        (void)write(STDERR_FILENO, buf, len);
    }
}

constexpr uint8_t FACE_NONE = 0;
constexpr uint8_t FACE_BITMAP = 1; // ordinary region: LiveInfo::markBitmap
constexpr uint8_t FACE_LARGE_FLAG = 2; // large region: metadata.isMarked

const char* FaceName(uint8_t face)
{
    switch (face) {
        case FACE_BITMAP:
            return "A_markBitmap";
        case FACE_LARGE_FLAG:
            return "B_largeIsMarked";
        default:
            return "none";
    }
}

const char* SourceName(uint8_t source)
{
    switch (source) {
        case 1:
            return "young";
        case 2:
            return "major";
        case 3:
            return "other";
        default:
            return "unknown";
    }
}

// One object, as its own region saw it at the last instant before the free.
struct ObjRow {
    uintptr_t obj = 0; // key; 0 = empty slot
    uintptr_t regionStart = 0;
    uint32_t freeSeq = 0;
    uint32_t gc = 0;
    uint32_t size = 0;
    uint32_t claimGc = 0;
    uint32_t majorGc = 0;
    uint16_t phase = 0;
    uint8_t path = 0;
    uint8_t face = FACE_NONE;
    uint8_t marked = 0;
    uint8_t survived = 0;
    uint8_t young = 0;
    uint8_t mbNull = 0;
    uint8_t epochStale = 0;
    uint8_t knownEmpty = 0;
    uint8_t neverExam = 0;
    uint8_t source = 0;    // claim ledger source, captured at free time
    uint8_t majorSkip = 0; // major saw wasMarked=true and skipped the field walk
    uint8_t hasRef = 0xff;
    uint8_t nonYoungRef = 0xff;
    uint8_t large = 0;
};

// The whole region, so an address that matches no object still gets an answer.
struct RegRow {
    uintptr_t start = 0;
    uintptr_t end = 0;
    uintptr_t allocPtr = 0;
    uint64_t markEpoch = 0;
    uint64_t snapEpoch = 0;
    uint64_t liveBytes = 0;
    uint32_t freeSeq = 0;
    uint32_t gc = 0;
    uint32_t walked = 0;
    uint32_t markedN = 0;
    uint32_t survivedN = 0;
    uint16_t phase = 0;
    uint8_t path = 0;
    uint8_t large = 0;
    uint8_t mbNull = 0;
    uint8_t rbNull = 0;
    uint8_t largeIsMarked = 0xff;
    uint8_t young = 0;
    uint8_t knownEmpty = 0;
    uint8_t neverExam = 0;
    uint8_t auth = 0;
    uint8_t rtype = 0;
    uint8_t truncated = 0;
    // Face-disagreement columns. Moving the sample earlier does not by itself make the
    // numerator reachable: for a large region IsMarkedObject(view,0) is
    // GetMarkedRegionFlag(view)==1 and IsSurvivedObject(view,0) is that OR isResurrected,
    // so marked implies survived. Every region the collector releases fails
    // !IsSurvivedObject(view,0), hence reads marked==0 through that same view - before
    // the predicate exactly as after it.
    //
    // What can be non-zero is a disagreement between faces. GetMarkedRegionFlag returns 0
    // outright when view.GetEpoch() != GetMarkSnapshotEpoch<G>() (RegionInfo.h:882-884),
    // and the predicate's GetMarkView<Old> need not carry the same epoch as the region's
    // route view. So record both reads plus the epochs, and let survivedSameView==0 select
    // the regions that were actually released.
    uint8_t markedSameView = 0xff; // predicate's view; must be 0 when survivedSameView==0
    uint8_t markedRoute = 0xff;    // route view; a 1 here on a released region is the bug
    uint8_t survivedSameView = 0xff;
    uint64_t viewEpoch = 0;
    uint64_t routeEpoch = 0;
    // Which face carried the mark, as data rather than as an assumption. The young face
    // is only readable on a young region (GetMarkView<Young> CHECKs otherwise), so 0xff
    // here means "not asked", never "not marked".
    uint8_t markedOldFace = 0xff;
    uint8_t markedYoungFace = 0xff;
    uint64_t youngEpoch = 0;
    uint8_t routeGenYoung = 0xff;
    uint8_t routeEpochLive = 0xff; // 0 = route's epoch gate is shut, so markedRoute is 0 by construction
};

constexpr size_t kRegCap = 1u << 15;
RegRow g_regs[kRegCap];
std::atomic<uint32_t> g_regNext{ 0 };
std::atomic<size_t> g_regTotal{ 0 };
std::atomic<size_t> g_regWrap{ 0 };

ObjRow* g_objs = nullptr;
size_t g_objCap = 0;
size_t g_maxObjPerRegion = 0;
bool g_walkObjects = true;
std::atomic<bool> g_objInit{ false };

std::atomic<size_t> g_objWalked{ 0 };
std::atomic<size_t> g_objRows{ 0 };
std::atomic<size_t> g_objOverwrite{ 0 };
std::atomic<size_t> g_markedInFreed{ 0 };
std::atomic<size_t> g_markedYoungSrcInFreed{ 0 };
std::atomic<size_t> g_walkTrunc{ 0 };
std::atomic<size_t> g_joinTry{ 0 };
std::atomic<size_t> g_joinExact{ 0 };
std::atomic<size_t> g_joinInterior{ 0 };
std::atomic<size_t> g_joinRegion{ 0 };
std::atomic<size_t> g_joinMiss{ 0 };
std::atomic<bool> g_healthOnce{ false };
std::atomic<bool> g_atexitOnce{ false };

// Per-path tallies. markedInFreed above is a single global that pools every path, and
// a numerator without its own denominator is not a population claim: "0 marked" reads
// the same whether nothing was marked or nothing was sampled. These split both halves
// by path so PATH_PRE_RELEASE_DECISION and PATH_LARGE_GARBAGE can be compared at all.
constexpr size_t kPathCap = 16;
std::atomic<size_t> g_pathRegs[kPathCap];
std::atomic<size_t> g_pathMarkedRegs[kPathCap];
std::atomic<size_t> g_pathObjs[kPathCap];
std::atomic<size_t> g_pathMarkedObjs[kPathCap];

// The four cells the verdict actually turns on, over large regions sampled at the
// pre-decision point. "Released" means the predicate that follows found the region not
// survived, which is survivedSameView==0 - no second call into RegionManager needed.
std::atomic<size_t> g_preLarge{ 0 };          // denominator: large regions sampled
std::atomic<size_t> g_preReleased{ 0 };       // of those, the ones the predicate then freed
std::atomic<size_t> g_preRelMarkedSame{ 0 };  // released AND marked via predicate's view (must be 0)
std::atomic<size_t> g_preRelMarkedRoute{ 0 }; // released AND marked via route view (the bug signature)
std::atomic<size_t> g_preEpochSplit{ 0 };     // released AND the two views disagree on epoch
// Positive control for the numerator's own expression. releasedMarkedRoute==0 is only a
// result if IsRouteMarkedObject can return 1 at all at this call site; if both of these
// were 0 the reported zero would be a dead probe, not a measurement.
std::atomic<size_t> g_preRetained{ 0 };
std::atomic<size_t> g_preRetMarkedRoute{ 0 };
std::atomic<size_t> g_preRetMarkedSame{ 0 };
// Released regions whose route-view epoch gate was shut. markedRoute is 0 for these by
// construction, so they must be excluded before a zero numerator is read as agreement.
std::atomic<size_t> g_preRelRouteEpochDead{ 0 };

// Positive control. A run in which nothing crashes produces joinTry=0, and a broken
// join path produces joinTry=0 as well. The self-test separates them by joining an
// address the instrument itself recorded (must hit exact), that address+8 (must hit
// interior), and an address no region ever covered (must miss).
std::atomic<uintptr_t> g_selfTestObj{ 0 };
std::atomic<bool> g_selfTestDone{ false };

bool SelfTestOn()
{
    static const bool on = []() { return EnvIsOne("MRT_GCV2_HOLDERCAP_SELFTEST"); }();
    return on;
}

// Single-threaded init is not guaranteed; the first caller wins and the rest spin
// until the pointer is published. Diagnostic path only.
std::atomic<int> g_initState{ 0 }; // 0 = untouched, 1 = building, 2 = ready

void EnsureStorage()
{
    int expected = 0;
    if (g_initState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        size_t cap = EnvSize("MRT_GCV2_HOLDERCAP_OBJCAP", 1u << 20);
        // round down to a power of two so the index mask is exact
        size_t pow2 = 1;
        while ((pow2 << 1) <= cap) {
            pow2 <<= 1;
        }
        g_maxObjPerRegion = EnvSize("MRT_GCV2_HOLDERCAP_MAXOBJ", 16384);
        const char* walk = std::getenv("MRT_GCV2_HOLDERCAP_WALK");
        g_walkObjects = !(walk != nullptr && std::strcmp(walk, "0") == 0);
        void* mem = std::calloc(pow2, sizeof(ObjRow));
        if (mem != nullptr) {
            g_objs = static_cast<ObjRow*>(mem);
            g_objCap = pow2;
        }
        g_initState.store(2, std::memory_order_release);
        return;
    }
    while (g_initState.load(std::memory_order_acquire) != 2) {
        // brief; only on the very first frees
    }
}

void HealthOnce()
{
    bool expected = false;
    if (g_healthOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        LOG(RTLOG_ERROR,
            "[GCV2][holdercap] health probe_live=1 env=MRT_GCV2_HOLDERCAP objCap=%zu maxObjPerRegion=%zu walk=%u",
            g_objCap, g_maxObjPerRegion, g_walkObjects ? 1U : 0U);
    }
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexitOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

size_t ObjIdx(uintptr_t obj)
{
    // objects are at least 8-byte aligned; mix the high bits back in
    uintptr_t h = (obj >> 3) ^ (obj >> 21);
    return static_cast<size_t>(h) & (g_objCap - 1);
}

void InsertObj(const ObjRow& row)
{
    if (g_objs == nullptr || g_objCap == 0 || row.obj == 0) {
        return;
    }
    size_t idx = ObjIdx(row.obj);
    for (size_t n = 0; n < 8; ++n) {
        size_t i = (idx + n) & (g_objCap - 1);
        if (g_objs[i].obj == row.obj || g_objs[i].obj == 0) {
            g_objs[i] = row;
            g_objRows.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // newest wins; count it so "not found" can never be read as "never happened"
    g_objs[idx] = row;
    g_objRows.fetch_add(1, std::memory_order_relaxed);
    g_objOverwrite.fetch_add(1, std::memory_order_relaxed);
}

const ObjRow* LookupObjExact(uintptr_t obj)
{
    if (g_objs == nullptr || g_objCap == 0 || obj == 0) {
        return nullptr;
    }
    size_t idx = ObjIdx(obj);
    for (size_t n = 0; n < 8; ++n) {
        size_t i = (idx + n) & (g_objCap - 1);
        if (g_objs[i].obj == obj) {
            return &g_objs[i];
        }
        if (g_objs[i].obj == 0) {
            return nullptr;
        }
    }
    return nullptr;
}

// Interior addresses (a field, a RawArray+8) do not hash to their object. Full scan
// is affordable exactly once, in the crash handler.
const ObjRow* LookupObjCovering(uintptr_t addr)
{
    if (g_objs == nullptr || g_objCap == 0 || addr == 0) {
        return nullptr;
    }
    const ObjRow* best = nullptr;
    for (size_t i = 0; i < g_objCap; ++i) {
        const ObjRow& row = g_objs[i];
        if (row.obj == 0 || row.size == 0) {
            continue;
        }
        if (addr >= row.obj && addr < row.obj + row.size) {
            if (best == nullptr || row.freeSeq > best->freeSeq) {
                best = &row;
            }
        }
    }
    return best;
}

const RegRow* LookupRegCovering(uintptr_t addr)
{
    if (addr == 0) {
        return nullptr;
    }
    size_t total = g_regTotal.load(std::memory_order_acquire);
    uint32_t next = g_regNext.load(std::memory_order_acquire);
    uint32_t n = static_cast<uint32_t>(total < kRegCap ? total : kRegCap);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t idx = (next + kRegCap - 1 - i) % kRegCap;
        if (total < kRegCap && idx >= total) {
            continue;
        }
        const RegRow& row = g_regs[idx];
        if (row.start == 0 || row.end <= row.start) {
            continue;
        }
        if (addr >= row.start && addr < row.end) {
            return &row;
        }
    }
    return nullptr;
}

void DumpObjRow(const char* tag, const char* kind, uintptr_t addr, const ObjRow& row)
{
    char line[768];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][holdercap] %s %s addr=%#zx obj=%#zx size=%u regionStart=%#zx "
                      "face=%s(%u) markedAtFree=%u survivedAtFree=%u source=%s(%u) claimGc=%u "
                      "majorSkip=%u majorGc=%u hasRef=%u nonYoungRef=%u young=%u large=%u "
                      "mbNull=%u epochStale=%u knownEmpty=%u neverExam=%u freeSeq=%u freeGc=%u "
                      "freePhase=%u path=%u\n",
                      tag != nullptr ? tag : "join", kind, addr, row.obj, row.size, row.regionStart,
                      FaceName(row.face), static_cast<unsigned>(row.face),
                      static_cast<unsigned>(row.marked), static_cast<unsigned>(row.survived),
                      SourceName(row.source), static_cast<unsigned>(row.source), row.claimGc,
                      static_cast<unsigned>(row.majorSkip), row.majorGc,
                      static_cast<unsigned>(row.hasRef), static_cast<unsigned>(row.nonYoungRef),
                      static_cast<unsigned>(row.young), static_cast<unsigned>(row.large),
                      static_cast<unsigned>(row.mbNull), static_cast<unsigned>(row.epochStale),
                      static_cast<unsigned>(row.knownEmpty), static_cast<unsigned>(row.neverExam),
                      row.freeSeq, row.gc, static_cast<unsigned>(row.phase),
                      static_cast<unsigned>(row.path));
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

void DumpRegRow(const char* tag, const char* kind, uintptr_t addr, const RegRow& row)
{
    char line[768];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][holdercap] %s %s addr=%#zx start=%#zx end=%#zx allocPtr=%#zx "
                      "large=%u largeIsMarked=%u mbNull=%u rbNull=%u markEpoch=%llu snapEpoch=%llu "
                      "walked=%u markedN=%u survivedN=%u trunc=%u live=%llu young=%u knownEmpty=%u "
                      "neverExam=%u auth=%u rtype=%u freeSeq=%u freeGc=%u freePhase=%u path=%u "
                      "markedSameView=%u survivedSameView=%u markedRoute=%u viewEpoch=%llu "
                      "routeEpoch=%llu markedOldFace=%u markedYoungFace=%u youngEpoch=%llu "
                      "routeGenYoung=%u routeEpochLive=%u\n",
                      tag != nullptr ? tag : "join", kind, addr, row.start, row.end, row.allocPtr,
                      static_cast<unsigned>(row.large), static_cast<unsigned>(row.largeIsMarked),
                      static_cast<unsigned>(row.mbNull), static_cast<unsigned>(row.rbNull),
                      static_cast<unsigned long long>(row.markEpoch),
                      static_cast<unsigned long long>(row.snapEpoch), row.walked, row.markedN,
                      row.survivedN, static_cast<unsigned>(row.truncated),
                      static_cast<unsigned long long>(row.liveBytes),
                      static_cast<unsigned>(row.young), static_cast<unsigned>(row.knownEmpty),
                      static_cast<unsigned>(row.neverExam), static_cast<unsigned>(row.auth),
                      static_cast<unsigned>(row.rtype), row.freeSeq, row.gc,
                      static_cast<unsigned>(row.phase), static_cast<unsigned>(row.path),
                      static_cast<unsigned>(row.markedSameView),
                      static_cast<unsigned>(row.survivedSameView),
                      static_cast<unsigned>(row.markedRoute),
                      static_cast<unsigned long long>(row.viewEpoch),
                      static_cast<unsigned long long>(row.routeEpoch),
                      static_cast<unsigned>(row.markedOldFace),
                      static_cast<unsigned>(row.markedYoungFace),
                      static_cast<unsigned long long>(row.youngEpoch),
                      static_cast<unsigned>(row.routeGenYoung),
                      static_cast<unsigned>(row.routeEpochLive));
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

// Which view the face is read through is load-bearing, not a style choice.
// GetMarkedRegionFlag(view) returns 0 outright when view.GetEpoch() is not the
// region's current mark-snapshot epoch (RegionInfo.h:882-884), so a sample taken
// through the wrong view answers 0 for a reason that has nothing to do with what the
// collectors left in the bit - which is exactly the constructive zero this instrument
// exists to remove.
//
// CollectLargeGarbage binds GetMarkView<Generation::Old>() and asks
// IsSurvivedObject(view, 0) (RegionManager.cpp:1725-1726). The pre-decision sample
// must therefore bind that same view, or the 1s and 0s it reports are about a
// different bit than the one the release decision is made on. Every other path is
// reached from CollectRegion, which consults the region's route view, so those keep
// reading the route face.
bool ReadMarked(RegionInfo* region, size_t offset, uint16_t path)
{
    if (path == RegionLifeDiag::PATH_PRE_RELEASE_DECISION) {
        return region->IsMarkedObject(region->GetMarkView<Generation::Old>(), offset);
    }
    return region->IsRouteMarkedObject(offset);
}

bool ReadSurvived(RegionInfo* region, size_t offset, uint16_t path)
{
    if (path == RegionLifeDiag::PATH_PRE_RELEASE_DECISION) {
        return region->IsSurvivedObject(region->GetMarkView<Generation::Old>(), offset);
    }
    return region->IsRouteSurvivedObject(offset);
}

// LiveInfo::markEpoch became a per-generation MarkFace epoch on main@6dd6a7d0. Same
// generation choice as ReadMarked, for the same reason: the epoch reported next to a
// sample has to be the epoch that sample was taken under.
// LiveInfo::GetMarkFace is private; the region's route view carries the same face
// epoch (RegionInfo.h GetRouteMarkView reads face->GetMarkFace<G>().epoch) through
// public API, so the diagnostic reads it there rather than widening a product type.
uint64_t MarkEpochOf(RegionInfo* region)
{
    if (region->GetRouteMarkGeneration() == Generation::Young) {
        return region->GetRouteMarkView<Generation::Young>().GetEpoch();
    }
    return region->GetRouteMarkView<Generation::Old>().GetEpoch();
}

uint16_t CurrentPhase()
{
    if (Runtime::CurrentRef() == nullptr) {
        return 0;
    }
    return static_cast<uint16_t>(Heap::GetHeap().GetGCPhase());
}

} // namespace

bool Enabled()
{
    return GateOn();
}

// The pre-decision sample. Everything the free-path snapshot records is still true
// here; what differs is only that CollectLargeGarbage has not yet asked whether the
// region survives, so metadata.isMarked has not been forced to 0 by our own gate.
// Recorded under its own path so the two are never pooled: a row from here answers
// "was it marked", a row from NoteRegionFree answers "was it marked after we
// established it was not", which is a question with one possible answer.
void NoteBeforeReleaseDecision(RegionInfo* region)
{
    NoteRegionFree(region, RegionLifeDiag::PATH_PRE_RELEASE_DECISION);
}

void NoteRegionFree(RegionInfo* region, uint16_t path)
{
    if (!GateOn() || region == nullptr) {
        return;
    }
    uintptr_t start = region->GetRegionStart();
    uintptr_t end = region->GetRegionEnd();
    if (start == 0 || end <= start) {
        return;
    }
    EnsureStorage();
    HealthOnce();
    EnsureAtexit();

    const bool large = region->IsLargeRegion();
    const bool knownEmpty = region->IsRouteKnownEmpty();
    RegionBitmap* mb = region->GetRouteMarkBitmap();
    RegionBitmap* rb = region->GetResurrectBitmap();
    LiveInfo* live = region->GetLiveInfo();
    const uint64_t snapEpoch = region->GetSnapshotEpoch();
    const uint64_t markEpoch = MarkEpochOf(region);
    const bool epochStale = live != nullptr && markEpoch != snapEpoch;
    const bool neverExam = knownEmpty && mb == nullptr && region->GetRegionAllocPtr() > start;

    uint32_t freeSeq = static_cast<uint32_t>(g_regTotal.fetch_add(1, std::memory_order_relaxed) + 1);
    uint32_t slotIdx = g_regNext.fetch_add(1, std::memory_order_relaxed);
    if (slotIdx >= kRegCap) {
        g_regWrap.fetch_add(1, std::memory_order_relaxed);
    }
    RegRow& reg = g_regs[slotIdx % kRegCap];
    reg.start = start;
    reg.end = end;
    reg.allocPtr = region->GetRegionAllocPtr();
    reg.markEpoch = markEpoch;
    reg.snapEpoch = snapEpoch;
    reg.liveBytes = region->GetLiveByteCount();
    reg.freeSeq = freeSeq;
    reg.gc = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    reg.phase = CurrentPhase();
    reg.path = static_cast<uint8_t>(path);
    reg.large = large ? 1 : 0;
    reg.mbNull = (mb == nullptr) ? 1 : 0;
    reg.rbNull = (rb == nullptr) ? 1 : 0;
    // A large region's mark state is a single flag rather than a bitmap, but it is
    // per-generation: the flag is scoped by MarkView<G>'s epoch, so "the" face is a
    // function of which view you bind. (Before the generational split there was only
    // one, and the comment that used to sit here still said so.)
    reg.largeIsMarked = large ? (ReadMarked(region, 0, path) ? 1 : 0) : 0xff;
    if (path == RegionLifeDiag::PATH_PRE_RELEASE_DECISION && large) {
        MarkView<Generation::Old> pview = region->GetMarkView<Generation::Old>();
        reg.markedSameView = region->IsMarkedObject(pview, static_cast<size_t>(0)) ? 1 : 0;
        reg.survivedSameView = region->IsSurvivedObject(pview, static_cast<size_t>(0)) ? 1 : 0;
        reg.markedRoute = region->IsRouteMarkedObject(static_cast<size_t>(0)) ? 1 : 0;
        reg.viewEpoch = pview.GetEpoch();
        reg.routeEpoch = MarkEpochOf(region);
        reg.markedOldFace = reg.markedSameView;
        // GetMarkView<Young> CHECKs !IsYoungRegion() and aborts, so the young face is
        // only legible when the region actually is young. 0xff means "not asked".
        if (region->IsYoungRegion()) {
            MarkView<Generation::Young> yview = region->GetMarkView<Generation::Young>();
            reg.markedYoungFace = region->IsMarkedObject(yview, static_cast<size_t>(0)) ? 1 : 0;
            reg.youngEpoch = yview.GetEpoch();
        }
        // Does the route read's epoch gate even open? GetMarkedRegionFlag returns 0 from
        // its first line when the view's epoch is not the region's current one, so a
        // markedRoute of 0 taken under a closed gate says nothing about the mark bit.
        // Without this column "the two faces agree" and "the second face was never
        // readable" are the same observation.
        reg.routeGenYoung = region->GetRouteMarkGeneration() == Generation::Young ? 1 : 0;
        const uint64_t routeLive = reg.routeGenYoung == 1
                                       ? region->GetMarkSnapshotEpoch<Generation::Young>()
                                       : region->GetMarkSnapshotEpoch<Generation::Old>();
        reg.routeEpochLive = (reg.routeEpoch == routeLive) ? 1 : 0;
    }
    reg.young = region->IsYoungRegion() ? 1 : 0;
    reg.knownEmpty = knownEmpty ? 1 : 0;
    reg.neverExam = neverExam ? 1 : 0;
    reg.auth = region->IsLiveCountAuthoritative() ? 1 : 0;
    reg.rtype = static_cast<uint8_t>(region->GetRegionType());
    reg.walked = 0;
    reg.markedN = 0;
    reg.survivedN = 0;
    reg.truncated = 0;

    if (g_walkObjects && g_objs != nullptr) {
        uint32_t walked = 0;
        uint32_t markedN = 0;
        uint32_t survivedN = 0;
        bool truncated = false;
        const uint8_t face = large ? FACE_LARGE_FLAG : FACE_BITMAP;
        region->VisitAllObjects([&](BaseObject* obj) {
            if (obj == nullptr) {
                return;
            }
            if (walked >= g_maxObjPerRegion) {
                truncated = true;
                return;
            }
            ++walked;
            uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
            size_t offset = addr - start;
            ObjRow row;
            row.obj = addr;
            row.regionStart = start;
            row.freeSeq = freeSeq;
            row.gc = reg.gc;
            row.size = static_cast<uint32_t>(obj->GetSize());
            row.phase = reg.phase;
            row.path = reg.path;
            row.face = face;
            row.marked = ReadMarked(region, offset, reg.path) ? 1 : 0;
            row.survived = ReadSurvived(region, offset, reg.path) ? 1 : 0;
            row.young = reg.young;
            row.large = reg.large;
            row.mbNull = reg.mbNull;
            row.epochStale = epochStale ? 1 : 0;
            row.knownEmpty = reg.knownEmpty;
            row.neverExam = reg.neverExam;
            // Freeze the youngclaim verdict here: the claim ring can wrap long before
            // the crash handler runs, and a wrapped ring reads exactly like "no claim".
            uint32_t claimGc = 0;
            uint32_t majorGc = 0;
            uint8_t majorSkip = 0;
            uint8_t hasRef = 0xff;
            uint8_t nonYoungRef = 0xff;
            row.source = HealPairDiag::LookupMarkOrigin(addr, &claimGc, &majorSkip, &majorGc, &hasRef,
                                                        &nonYoungRef);
            row.claimGc = claimGc;
            row.majorGc = majorGc;
            row.majorSkip = majorSkip;
            row.hasRef = hasRef;
            row.nonYoungRef = nonYoungRef;
            if (row.marked != 0) {
                ++markedN;
                g_markedInFreed.fetch_add(1, std::memory_order_relaxed);
                if (row.source == 1) {
                    g_markedYoungSrcInFreed.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (row.survived != 0) {
                ++survivedN;
            }
            InsertObj(row);
            if (SelfTestOn() && row.size > 8 && g_selfTestObj.load(std::memory_order_relaxed) == 0) {
                g_selfTestObj.store(addr, std::memory_order_relaxed);
            }
        });
        g_objWalked.fetch_add(walked, std::memory_order_relaxed);
        if (truncated) {
            g_walkTrunc.fetch_add(1, std::memory_order_relaxed);
        }
        reg.walked = walked;
        reg.markedN = markedN;
        reg.survivedN = survivedN;
        reg.truncated = truncated ? 1 : 0;
    }

    if (path == RegionLifeDiag::PATH_PRE_RELEASE_DECISION && large) {
        g_preLarge.fetch_add(1, std::memory_order_relaxed);
        if (reg.survivedSameView == 0) {
            g_preReleased.fetch_add(1, std::memory_order_relaxed);
            if (reg.markedSameView == 1) {
                g_preRelMarkedSame.fetch_add(1, std::memory_order_relaxed);
            }
            if (reg.markedRoute == 1) {
                g_preRelMarkedRoute.fetch_add(1, std::memory_order_relaxed);
            }
            if (reg.viewEpoch != reg.routeEpoch) {
                g_preEpochSplit.fetch_add(1, std::memory_order_relaxed);
            }
            // The false negative to rule out before calling a zero "the faces agree".
            if (reg.routeEpochLive == 0) {
                g_preRelRouteEpochDead.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            g_preRetained.fetch_add(1, std::memory_order_relaxed);
            if (reg.markedRoute == 1) {
                g_preRetMarkedRoute.fetch_add(1, std::memory_order_relaxed);
            }
            if (reg.markedSameView == 1) {
                g_preRetMarkedSame.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Per-path denominator. Counted for every call, not only the dumped ones.
    if (path < kPathCap) {
        g_pathRegs[path].fetch_add(1, std::memory_order_relaxed);
        g_pathObjs[path].fetch_add(reg.walked, std::memory_order_relaxed);
        g_pathMarkedObjs[path].fetch_add(reg.markedN, std::memory_order_relaxed);
        if (reg.markedN > 0) {
            g_pathMarkedRegs[path].fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Bounded sample so the log carries shape without drowning the recipe. A released
    // region whose route view still says marked is never sampled away - that row is the
    // whole point of the instrument.
    const bool faceSplit = reg.survivedSameView == 0 && reg.markedRoute == 1;
    if (freeSeq <= 32 || (freeSeq & (freeSeq - 1)) == 0 || reg.markedN > 0 || faceSplit) {
        const char* kind = faceSplit ? "region_released_route_marked"
                                     : (reg.markedN > 0 ? "region_marked_live" : "region");
        DumpRegRow("free", kind, start, reg);
    }
}

void DumpJoinForAddr(uintptr_t addr, const char* tag, bool deepScan)
{
    if (!GateOn() || addr == 0) {
        return;
    }
    g_joinTry.fetch_add(1, std::memory_order_relaxed);
    const ObjRow* exact = LookupObjExact(addr);
    if (exact != nullptr) {
        g_joinExact.fetch_add(1, std::memory_order_relaxed);
        DumpObjRow(tag, "exact", addr, *exact);
        Report("join_exact");
        return;
    }
    const ObjRow* cover = deepScan ? LookupObjCovering(addr) : nullptr;
    if (cover != nullptr) {
        g_joinInterior.fetch_add(1, std::memory_order_relaxed);
        DumpObjRow(tag, "interior", addr, *cover);
        Report("join_interior");
        return;
    }
    const RegRow* reg = LookupRegCovering(addr);
    if (reg != nullptr) {
        g_joinRegion.fetch_add(1, std::memory_order_relaxed);
        DumpRegRow(tag, "region_only", addr, *reg);
        Report("join_region");
        return;
    }
    g_joinMiss.fetch_add(1, std::memory_order_relaxed);
    char miss[256];
    int n = sprintf_s(miss, sizeof(miss),
                      "[GCV2][holdercap] %s miss addr=%#zx regTotal=%zu regWrap=%zu objRows=%zu "
                      "(address was never inside a region this process freed)\n",
                      tag != nullptr ? tag : "join", addr, g_regTotal.load(std::memory_order_relaxed),
                      g_regWrap.load(std::memory_order_relaxed), g_objRows.load(std::memory_order_relaxed));
    if (n > 0) {
        WriteLine(miss, static_cast<size_t>(n));
    }
    Report("join_miss");
}

void NoteCrashSweep(const uintptr_t* addrs, const char* const* names, size_t n)
{
    if (!GateOn() || addrs == nullptr || names == nullptr || n == 0) {
        return;
    }
    constexpr size_t kMaxCand = 16;
    if (n > kMaxCand) {
        n = kMaxCand;
    }
    const ObjRow* best[kMaxCand] = {};
    uint8_t exact[kMaxCand] = {};
    size_t hits = 0;

    // Exact lookups first: cheap, and they are the answer whenever a register
    // holds the object itself rather than a field inside it.
    for (size_t c = 0; c < n; ++c) {
        if (addrs[c] == 0) {
            continue;
        }
        const ObjRow* row = LookupObjExact(addrs[c]);
        if (row != nullptr) {
            best[c] = row;
            exact[c] = 1;
        }
    }
    // One pass for every still-unresolved candidate. Interiors resolve here.
    if (g_objs != nullptr && g_objCap != 0) {
        for (size_t i = 0; i < g_objCap; ++i) {
            const ObjRow& row = g_objs[i];
            if (row.obj == 0 || row.size == 0) {
                continue;
            }
            for (size_t c = 0; c < n; ++c) {
                if (addrs[c] == 0 || exact[c] != 0) {
                    continue;
                }
                if (addrs[c] >= row.obj && addrs[c] < row.obj + row.size) {
                    if (best[c] == nullptr || row.freeSeq > best[c]->freeSeq) {
                        best[c] = &row;
                    }
                }
            }
        }
    }
    for (size_t c = 0; c < n; ++c) {
        if (addrs[c] == 0) {
            continue;
        }
        g_joinTry.fetch_add(1, std::memory_order_relaxed);
        if (best[c] != nullptr) {
            ++hits;
            if (exact[c] != 0) {
                g_joinExact.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_joinInterior.fetch_add(1, std::memory_order_relaxed);
            }
            DumpObjRow("crash_sweep", exact[c] != 0 ? "exact" : "interior", addrs[c], *best[c]);
            continue;
        }
        const RegRow* reg = LookupRegCovering(addrs[c]);
        if (reg != nullptr) {
            ++hits;
            g_joinRegion.fetch_add(1, std::memory_order_relaxed);
            DumpRegRow("crash_sweep", "region_only", addrs[c], *reg);
            continue;
        }
        g_joinMiss.fetch_add(1, std::memory_order_relaxed);
    }
    // Census line: candidates offered, and which name carried each hit. Without it,
    // "no hit line" and "the sweep never ran" are the same observation.
    char head[512];
    int hn = sprintf_s(head, sizeof(head), "[GCV2][holdercap] crash_sweep census cand=%zu hits=%zu names=",
                       n, hits);
    if (hn > 0) {
        size_t used = static_cast<size_t>(hn);
        for (size_t c = 0; c < n && used + 24 < sizeof(head); ++c) {
            const char* verdict = (best[c] != nullptr) ? "obj" : "-";
            int an = sprintf_s(head + used, sizeof(head) - used, "%s%s=%s", c == 0 ? "" : ",",
                               names[c] != nullptr ? names[c] : "?", verdict);
            if (an <= 0) {
                break;
            }
            used += static_cast<size_t>(an);
        }
        if (used + 2 < sizeof(head)) {
            head[used++] = '\n';
            head[used] = '\0';
        }
        WriteLine(head, used);
    }
    Report("crash_sweep");
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    if (SelfTestOn() && point != nullptr && std::strcmp(point, "gc_end") == 0) {
        uintptr_t probe = g_selfTestObj.load(std::memory_order_relaxed);
        bool expected = false;
        if (probe != 0 && g_selfTestDone.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            char banner[192];
            int bn = sprintf_s(banner, sizeof(banner),
                               "[GCV2][holdercap] selftest begin probe=%#zx (expect exact, interior, miss)\n",
                               probe);
            if (bn > 0) {
                WriteLine(banner, static_cast<size_t>(bn));
            }
            DumpJoinForAddr(probe, "selftest_exact", true);
            DumpJoinForAddr(probe + 8, "selftest_interior", true);
            DumpJoinForAddr(static_cast<uintptr_t>(0xdead0000ULL), "selftest_miss", true);
        }
    }
    char line[512];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][holdercap] point=%s regTotal=%zu regWrap=%zu objWalked=%zu objRows=%zu "
                      "objOverwrite=%zu markedInFreed=%zu markedYoungSrcInFreed=%zu walkTrunc=%zu "
                      "joinTry=%zu joinExact=%zu joinInterior=%zu joinRegion=%zu joinMiss=%zu "
                      "objCap=%zu\n",
                      point != nullptr ? point : "?", g_regTotal.load(std::memory_order_relaxed),
                      g_regWrap.load(std::memory_order_relaxed), g_objWalked.load(std::memory_order_relaxed),
                      g_objRows.load(std::memory_order_relaxed), g_objOverwrite.load(std::memory_order_relaxed),
                      g_markedInFreed.load(std::memory_order_relaxed),
                      g_markedYoungSrcInFreed.load(std::memory_order_relaxed),
                      g_walkTrunc.load(std::memory_order_relaxed), g_joinTry.load(std::memory_order_relaxed),
                      g_joinExact.load(std::memory_order_relaxed), g_joinInterior.load(std::memory_order_relaxed),
                      g_joinRegion.load(std::memory_order_relaxed), g_joinMiss.load(std::memory_order_relaxed),
                      g_objCap);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }

    // The verdict line. preReleased is the denominator that matters: a zero numerator
    // over a zero denominator is what the previous round reported, so both are printed
    // side by side and neither can be quoted without the other.
    char pre[320];
    int prn = sprintf_s(pre, sizeof(pre),
                        "[GCV2][holdercap] predecision point=%s largeSampled=%zu released=%zu "
                        "releasedMarkedSameView=%zu releasedMarkedRoute=%zu releasedEpochSplit=%zu "
                        "retained=%zu retainedMarkedSameView=%zu retainedMarkedRoute=%zu "
                        "releasedRouteEpochDead=%zu\n",
                        point != nullptr ? point : "?", g_preLarge.load(std::memory_order_relaxed),
                        g_preReleased.load(std::memory_order_relaxed),
                        g_preRelMarkedSame.load(std::memory_order_relaxed),
                        g_preRelMarkedRoute.load(std::memory_order_relaxed),
                        g_preEpochSplit.load(std::memory_order_relaxed),
                        g_preRetained.load(std::memory_order_relaxed),
                        g_preRetMarkedSame.load(std::memory_order_relaxed),
                        g_preRetMarkedRoute.load(std::memory_order_relaxed),
                        g_preRelRouteEpochDead.load(std::memory_order_relaxed));
    if (prn > 0) {
        WriteLine(pre, static_cast<size_t>(prn));
    }

    // One line per path that saw traffic. A path with regs=0 is omitted rather than
    // printed as zero, so "absent" and "sampled nothing" stay distinguishable.
    for (size_t p = 0; p < kPathCap; ++p) {
        size_t regs = g_pathRegs[p].load(std::memory_order_relaxed);
        if (regs == 0) {
            continue;
        }
        char pl[256];
        int pn = sprintf_s(pl, sizeof(pl),
                           "[GCV2][holdercap] pathdist point=%s path=%zu regs=%zu markedRegs=%zu "
                           "objs=%zu markedObjs=%zu\n",
                           point != nullptr ? point : "?", p, regs,
                           g_pathMarkedRegs[p].load(std::memory_order_relaxed),
                           g_pathObjs[p].load(std::memory_order_relaxed),
                           g_pathMarkedObjs[p].load(std::memory_order_relaxed));
        if (pn > 0) {
            WriteLine(pl, static_cast<size_t>(pn));
        }
    }
}

} // namespace MarkFaceSnap
} // namespace MapleRuntime
