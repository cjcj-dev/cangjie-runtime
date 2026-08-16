// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/MarkFaceSnap.h"

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
                      "neverExam=%u auth=%u rtype=%u freeSeq=%u freeGc=%u freePhase=%u path=%u\n",
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
                      static_cast<unsigned>(row.phase), static_cast<unsigned>(row.path));
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
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
    const bool knownEmpty = region->IsKnownEmpty();
    RegionBitmap* mb = region->GetMarkBitmap();
    RegionBitmap* rb = region->GetResurrectBitmap();
    LiveInfo* live = region->GetLiveInfo();
    const uint64_t snapEpoch = region->GetSnapshotEpoch();
    const uint64_t markEpoch = live != nullptr ? live->markEpoch : 0;
    const bool epochStale = live != nullptr && live->markEpoch != snapEpoch;
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
    // face B is the only mark face a large region has; read it as such, never the bitmap.
    reg.largeIsMarked = large ? (region->IsMarkedObject(static_cast<size_t>(0)) ? 1 : 0) : 0xff;
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
            row.marked = region->IsMarkedObject(offset) ? 1 : 0;
            row.survived = region->IsSurvivedObject(offset) ? 1 : 0;
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

    // Bounded sample so the log carries shape without drowning the recipe.
    if (freeSeq <= 32 || (freeSeq & (freeSeq - 1)) == 0 || reg.markedN > 0) {
        DumpRegRow("free", reg.markedN > 0 ? "region_marked_live" : "region", start, reg);
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

void Report(const char* point)
{
    if (!GateOn()) {
        return;
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
}

} // namespace MarkFaceSnap
} // namespace MapleRuntime
