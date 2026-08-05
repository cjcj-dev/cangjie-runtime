// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "NextField2Probe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "Common/BaseObject.h"
#include "Common/StateWord.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/MClass.inline.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {

#define NF2_LOG(fmt, ...)                                                                                              \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][nextfield2] " fmt "\n", ##__VA_ARGS__);                                            \
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

uint64_t NowNs()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

bool PageMapped(uintptr_t addr)
{
    if (addr == 0) {
        return false;
    }
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        pageSize = 4096;
    }
    uintptr_t page = addr & ~(static_cast<uintptr_t>(pageSize) - 1);
    unsigned char vec = 0;
    if (mincore(reinterpret_cast<void*>(page), static_cast<size_t>(pageSize), &vec) == 0) {
        return true;
    }
    return false;
}

TypeInfo* PeekTypeInfoAt(uintptr_t addr)
{
    if (addr == 0 || (addr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return nullptr;
    }
    if (!Heap::IsHeapAddress(addr)) {
        return nullptr;
    }
#ifdef __arm__
    uint32_t raw = 0;
    std::memcpy(&raw, reinterpret_cast<const void*>(addr), sizeof(raw));
    return reinterpret_cast<TypeInfo*>(static_cast<uintptr_t>(raw));
#else
    uint32_t low = 0;
    uint16_t high = 0;
    std::memcpy(&low, reinterpret_cast<const void*>(addr), sizeof(low));
    std::memcpy(&high, reinterpret_cast<const void*>(addr + 4), sizeof(high));
    uintptr_t tipAddr = (static_cast<uintptr_t>(high) << 32) | static_cast<uintptr_t>(low);
    return reinterpret_cast<TypeInfo*>(tipAddr);
#endif
}

// Same TipLooksValid as remsetholder / interiorwriter (POSCTRL 405).
bool TipLooksValid(TypeInfo* tip)
{
    if (tip == nullptr) {
        return false;
    }
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return false;
    }
    if (Heap::IsHeapAddress(tipAddr)) {
        return false;
    }
    bool inTim = TypeInfoManager::GetTypeInfoManager().ContainsAddress(tipAddr);
    if (!inTim && !PageMapped(tipAddr)) {
        return false;
    }
    if (!tip->IsVaildType()) {
        return false;
    }
    MSize isz = tip->GetInstanceSize();
    if (isz == 0 || isz > (1u << 20)) {
        return false;
    }
    return true;
}

size_t SaneObjectSize(TypeInfo* tip, RegionInfo* region)
{
    if (tip == nullptr || region == nullptr) {
        return 0;
    }
    if (tip->IsRawArray()) {
        return 0;
    }
    MSize isz = tip->GetInstanceSize();
    size_t size = (static_cast<size_t>(isz) + 8u + 7u) & ~static_cast<size_t>(7u);
    size_t regionBytes = region->GetRegionEnd() - region->GetRegionStart();
    if (size < 16 || size > regionBytes || size > (1u << 20)) {
        return 0;
    }
    return size;
}

enum class Kind : uint8_t { Null = 0, Base = 1, Interior = 2, Unknown = 3, NotHeap = 4 };

const char* KindName(Kind k)
{
    switch (k) {
        case Kind::Null:
            return "null";
        case Kind::Base:
            return "base";
        case Kind::Interior:
            return "interior";
        case Kind::Unknown:
            return "unknown";
        case Kind::NotHeap:
            return "not_heap";
        default:
            return "?";
    }
}

void Classify(uintptr_t value, Kind& kind, uintptr_t& baseOut, size_t& offsetOut, TypeInfo*& tipAtValue,
              TypeInfo*& tipAtBase)
{
    kind = Kind::Unknown;
    baseOut = 0;
    offsetOut = 0;
    tipAtValue = nullptr;
    tipAtBase = nullptr;

    if (value == 0) {
        kind = Kind::Null;
        return;
    }
    if (!Heap::IsHeapAddress(value)) {
        kind = Kind::NotHeap;
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(value);
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        kind = Kind::Unknown;
        return;
    }

    tipAtValue = PeekTypeInfoAt(value);
    if (TipLooksValid(tipAtValue)) {
        kind = Kind::Base;
        baseOut = value;
        offsetOut = 0;
        tipAtBase = tipAtValue;
        return;
    }

    static const size_t kOffs[] = {8, 16, 24, 32};
    for (size_t off : kOffs) {
        if (value < off) {
            continue;
        }
        uintptr_t cand = value - off;
        if (!Heap::IsHeapAddress(cand)) {
            continue;
        }
        RegionInfo* candRegion = RegionInfo::TryGetRegionInfoAt(cand);
        if (candRegion != region) {
            continue;
        }
        TypeInfo* tip = PeekTypeInfoAt(cand);
        if (!TipLooksValid(tip)) {
            continue;
        }
        size_t size = SaneObjectSize(tip, region);
        bool sizeOk = false;
        if (size != 0 && value >= cand && value < cand + size && cand + size <= region->GetRegionEnd()) {
            sizeOk = true;
        }
        if (!sizeOk && size == 0) {
            if (off == 8 || off == 16 || off == 24) {
                sizeOk = true;
            }
        }
        if (!sizeOk) {
            continue;
        }
        kind = Kind::Interior;
        baseOut = cand;
        offsetOut = off;
        tipAtBase = tip;
        return;
    }
    kind = Kind::Unknown;
}

// Cache of TypeInfo* for default:Node (remsetholder tipVal=0x…74fc20 family).
// Once resolved by name, subsequent matches are pointer equality only (cheap + safe).
std::atomic<TypeInfo*> gNodeTip{nullptr};
std::atomic<uint64_t> gRejAlign{0};
std::atomic<uint64_t> gRejNotHeap{0};
std::atomic<uint64_t> gRejRegion{0};
std::atomic<uint64_t> gRejTip{0};
std::atomic<uint64_t> gRejSize{0};
std::atomic<uint64_t> gRejName{0};
std::atomic<uint64_t> gCandTipValid{0};

bool ResolveAsNodeTip(TypeInfo* tip)
{
    if (tip == nullptr) {
        return false;
    }
    TypeInfo* cached = gNodeTip.load(std::memory_order_acquire);
    if (cached != nullptr) {
        return tip == cached;
    }
    if (!TipLooksValid(tip)) {
        return false;
    }
    // Node: instanceSize 40 ⇒ object size 48 (remsetholder holderSize=48).
    MSize isz = tip->GetInstanceSize();
    if (isz != 40 && isz != 48) {
        return false;
    }
    const char* name = tip->GetName();
    if (name == nullptr) {
        return false;
    }
    if (!PageMapped(reinterpret_cast<uintptr_t>(name))) {
        return false;
    }
    if (std::strcmp(name, "default:Node") != 0) {
        return false;
    }
    TypeInfo* expected = nullptr;
    gNodeTip.compare_exchange_strong(expected, tip, std::memory_order_acq_rel);
    return true;
}

// Cheap gate: slot == holder+16 and holder TypeInfo is default:Node.
bool IsNodeNextSlot(uintptr_t slotU, uintptr_t& holderOut, TypeInfo*& holderTipOut)
{
    holderOut = 0;
    holderTipOut = nullptr;
    if ((slotU & 0x7) != 0 || slotU < 16) {
        gRejAlign.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!Heap::IsHeapAddress(slotU)) {
        gRejNotHeap.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    uintptr_t holder = slotU - 16;
    if ((holder & 0x7) != 0 || !Heap::IsHeapAddress(holder)) {
        gRejNotHeap.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(holder);
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        gRejRegion.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    TypeInfo* tip = PeekTypeInfoAt(holder);
    if (tip == nullptr) {
        gRejTip.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Fast path: tip pointer already known as Node.
    TypeInfo* cached = gNodeTip.load(std::memory_order_acquire);
    if (cached != nullptr && tip == cached) {
        holderOut = holder;
        holderTipOut = tip;
        return true;
    }
    // Slow path: validate tip as Node (once).
    if (!ResolveAsNodeTip(tip)) {
        // Count near-misses: tip looks valid but not Node.
        if (TipLooksValid(tip)) {
            gCandTipValid.fetch_add(1, std::memory_order_relaxed);
            MSize isz = tip->GetInstanceSize();
            if (isz == 40 || isz == 48) {
                gRejName.fetch_add(1, std::memory_order_relaxed);
            } else {
                gRejSize.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            gRejTip.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }
    holderOut = holder;
    holderTipOut = tip;
    return true;
}

thread_local const char* gInstallPath = "sink";

std::atomic<uint64_t> gSeenInstalls{0};     // all NoteInstall calls (cheap counter)
std::atomic<uint64_t> gNodeHits{0};         // Node@+16 matches (full classify)
std::atomic<uint64_t> gNull{0};
std::atomic<uint64_t> gBase{0};
std::atomic<uint64_t> gInterior{0};
std::atomic<uint64_t> gUnknown{0};
std::atomic<uint64_t> gNotHeap{0};
std::atomic<uint64_t> gDumpLeft{0};
std::atomic<bool> gArmedLogged{false};

// Per-path counts for Node@+16 only.
constexpr int kMaxPaths = 16;
struct PathSlot {
    std::atomic<const char*> name{nullptr};
    std::atomic<uint64_t> total{0};
    std::atomic<uint64_t> base{0};
    std::atomic<uint64_t> interior{0};
};
PathSlot gPaths[kMaxPaths];

int PathIndex(const char* path)
{
    if (path == nullptr) {
        path = "null";
    }
    for (int i = 0; i < kMaxPaths; ++i) {
        const char* n = gPaths[i].name.load(std::memory_order_acquire);
        if (n == path || (n != nullptr && std::strcmp(n, path) == 0)) {
            return i;
        }
        if (n == nullptr) {
            const char* expected = nullptr;
            if (gPaths[i].name.compare_exchange_strong(expected, path, std::memory_order_acq_rel)) {
                return i;
            }
            n = gPaths[i].name.load(std::memory_order_acquire);
            if (n != nullptr && (n == path || std::strcmp(n, path) == 0)) {
                return i;
            }
        }
    }
    return 0;
}

// Write history ring: track last K writes per slot (hash table).
constexpr size_t kHistSlots = 1024;
constexpr size_t kHistDepth = 8;

struct HistEntry {
    uint64_t ns;
    uintptr_t value;
    uint8_t kind; // Kind
    uint8_t pathIdx;
    uint16_t phase;
};

struct HistBucket {
    std::atomic<uintptr_t> slot{0}; // 0 = empty
    std::atomic<uint32_t> writeCount{0};
    std::atomic<uint32_t> head{0}; // next write index
    HistEntry ring[kHistDepth];
    std::atomic<uint64_t> lastBaseNs{0};
    std::atomic<uint64_t> lastInteriorNs{0};
    std::atomic<uintptr_t> lastValue{0};
    std::atomic<uint8_t> lastKind{0};
};

HistBucket gHist[kHistSlots];
std::atomic<uint64_t> gHistOverflow{0};

HistBucket* FindOrAllocHist(uintptr_t slot)
{
    size_t h = (slot >> 3) % kHistSlots;
    for (size_t i = 0; i < kHistSlots; ++i) {
        size_t idx = (h + i) % kHistSlots;
        uintptr_t cur = gHist[idx].slot.load(std::memory_order_acquire);
        if (cur == slot) {
            return &gHist[idx];
        }
        if (cur == 0) {
            uintptr_t expected = 0;
            if (gHist[idx].slot.compare_exchange_strong(expected, slot, std::memory_order_acq_rel)) {
                return &gHist[idx];
            }
            if (expected == slot) {
                return &gHist[idx];
            }
        }
    }
    gHistOverflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void RecordHist(HistBucket* b, uint64_t ns, uintptr_t value, Kind k, int pathIdx, GCPhase phase)
{
    if (b == nullptr) {
        return;
    }
    uint32_t head = b->head.fetch_add(1, std::memory_order_relaxed);
    uint32_t i = head % kHistDepth;
    b->ring[i].ns = ns;
    b->ring[i].value = value;
    b->ring[i].kind = static_cast<uint8_t>(k);
    b->ring[i].pathIdx = static_cast<uint8_t>(pathIdx & 0xff);
    b->ring[i].phase = static_cast<uint16_t>(phase);
    b->writeCount.fetch_add(1, std::memory_order_relaxed);
    b->lastValue.store(value, std::memory_order_relaxed);
    b->lastKind.store(static_cast<uint8_t>(k), std::memory_order_relaxed);
    if (k == Kind::Base) {
        b->lastBaseNs.store(ns, std::memory_order_relaxed);
    } else if (k == Kind::Interior) {
        b->lastInteriorNs.store(ns, std::memory_order_relaxed);
    }
}

void DumpHistBucket(const HistBucket& b, const char* why)
{
    uintptr_t slot = b.slot.load(std::memory_order_relaxed);
    if (slot == 0) {
        return;
    }
    uint32_t wc = b.writeCount.load(std::memory_order_relaxed);
    uint32_t head = b.head.load(std::memory_order_relaxed);
    NF2_LOG("HIST why=%s slot=%p writes=%u lastValue=%p lastKind=%s lastBaseNs=%llu lastInteriorNs=%llu", why,
            reinterpret_cast<void*>(slot), wc, reinterpret_cast<void*>(b.lastValue.load(std::memory_order_relaxed)),
            KindName(static_cast<Kind>(b.lastKind.load(std::memory_order_relaxed))),
            static_cast<unsigned long long>(b.lastBaseNs.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(b.lastInteriorNs.load(std::memory_order_relaxed)));
    size_t n = wc < kHistDepth ? wc : kHistDepth;
    // head is next-to-write; oldest of retained is head-n
    for (size_t j = 0; j < n; ++j) {
        size_t idx = (head + kHistDepth - n + j) % kHistDepth;
        const HistEntry& e = b.ring[idx];
        const char* pname = "?";
        if (e.pathIdx < kMaxPaths) {
            const char* pn = gPaths[e.pathIdx].name.load(std::memory_order_relaxed);
            if (pn != nullptr) {
                pname = pn;
            }
        }
        NF2_LOG("HIST_ENTRY slot=%p i=%zu ns=%llu value=%p vkind=%s path=%s phase=%u",
                reinterpret_cast<void*>(slot), j, static_cast<unsigned long long>(e.ns),
                reinterpret_cast<void*>(e.value), KindName(static_cast<Kind>(e.kind)), pname,
                static_cast<unsigned>(e.phase));
    }
}

} // namespace

bool NextField2Probe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_NEXTFIELD2");
    return on;
}

const char* NextField2Probe::CurrentPath()
{
    return gInstallPath != nullptr ? gInstallPath : "sink";
}

NextField2Probe::ScopedInstallPath::ScopedInstallPath(const char* path) : prev_(gInstallPath)
{
    gInstallPath = path != nullptr ? path : "null";
}

NextField2Probe::ScopedInstallPath::~ScopedInstallPath()
{
    gInstallPath = prev_;
}

void NextField2Probe::NoteInstall(const char* path, const char* kind, void* slot, void* value)
{
    if (!Enabled()) {
        return;
    }

    gSeenInstalls.fetch_add(1, std::memory_order_relaxed);

    if (!gArmedLogged.exchange(true, std::memory_order_relaxed)) {
        size_t dumpMax = EnvSizeT("MRT_GCV2_NEXTFIELD2_DUMP_MAX", 256);
        gDumpLeft.store(dumpMax, std::memory_order_relaxed);
        NF2_LOG("ARMED env=MRT_GCV2_NEXTFIELD2=1 dumpMax=%zu (Node@+16 full coverage, no sample)", dumpMax);
    }

    uintptr_t slotU = reinterpret_cast<uintptr_t>(slot);
    uintptr_t holder = 0;
    TypeInfo* holderTip = nullptr;
    if (!IsNodeNextSlot(slotU, holder, holderTip)) {
        return; // not our field — cheap reject, no Classify
    }

    const char* pathStr = path;
    if (pathStr == nullptr || pathStr[0] == '\0' || std::strcmp(pathStr, "sink") == 0) {
        pathStr = CurrentPath();
    }
    const char* kindStr = kind != nullptr ? kind : "?";

    uintptr_t val = reinterpret_cast<uintptr_t>(value);
    Kind k = Kind::Unknown;
    uintptr_t base = 0;
    size_t offset = 0;
    TypeInfo* tipVal = nullptr;
    TypeInfo* tipBase = nullptr;
    Classify(val, k, base, offset, tipVal, tipBase);

    gNodeHits.fetch_add(1, std::memory_order_relaxed);
    int pidx = PathIndex(pathStr);
    gPaths[pidx].total.fetch_add(1, std::memory_order_relaxed);

    switch (k) {
        case Kind::Null:
            gNull.fetch_add(1, std::memory_order_relaxed);
            break;
        case Kind::Base:
            gBase.fetch_add(1, std::memory_order_relaxed);
            gPaths[pidx].base.fetch_add(1, std::memory_order_relaxed);
            break;
        case Kind::Interior:
            gInterior.fetch_add(1, std::memory_order_relaxed);
            gPaths[pidx].interior.fetch_add(1, std::memory_order_relaxed);
            break;
        case Kind::NotHeap:
            gNotHeap.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            gUnknown.fetch_add(1, std::memory_order_relaxed);
            break;
    }

    GCPhase phase = Heap::GetHeap().GetGCPhase();
    uint64_t ns = NowNs();
    HistBucket* hb = FindOrAllocHist(slotU);
    RecordHist(hb, ns, val, k, pidx, phase);

    // Dump: all interiors; sample first few bases for posctrl; all unknowns.
    bool dump = false;
    if (k == Kind::Interior) {
        dump = true;
    } else if (k == Kind::Unknown) {
        dump = true;
    } else if (k == Kind::Base) {
        uint64_t b = gBase.load(std::memory_order_relaxed);
        if (b <= 8 || (b & 0x3ff) == 0) { // first 8 + 1/1024 sample
            dump = true;
        }
    } else if (k == Kind::Null) {
        uint64_t n = gNull.load(std::memory_order_relaxed);
        if (n <= 4) {
            dump = true;
        }
    }
    if (!dump) {
        return;
    }
    uint64_t left = gDumpLeft.load(std::memory_order_relaxed);
    while (left > 0) {
        if (gDumpLeft.compare_exchange_weak(left, left - 1, std::memory_order_relaxed)) {
            break;
        }
    }
    if (left == 0) {
        return;
    }

    const char* phaseName = Collector::GetGCPhaseName(phase);
    void* ra0 = __builtin_return_address(0);
    void* ra1 = __builtin_frame_address(1) != nullptr ? __builtin_return_address(1) : nullptr;
    void* ra2 = __builtin_frame_address(2) != nullptr ? __builtin_return_address(2) : nullptr;
    void* ra3 = nullptr;

    const char* tipBaseName = "?";
    if (tipBase != nullptr && TipLooksValid(tipBase)) {
        tipBaseName = tipBase->GetName();
    }
    const char* holderName = holderTip != nullptr ? holderTip->GetName() : "?";

    // Region state of value (for lifecycle hypothesis).
    unsigned valFree = 0, valGarbage = 0, valYoung = 0;
    if (val != 0 && Heap::IsHeapAddress(val)) {
        RegionInfo* vr = RegionInfo::TryGetRegionInfoAt(val);
        if (vr != nullptr) {
            valFree = vr->IsFreeRegion() ? 1u : 0u;
            valGarbage = vr->IsGarbageRegion() ? 1u : 0u;
            valYoung = vr->IsYoungRegion() ? 1u : 0u;
        }
    }
    unsigned holderFree = 0, holderGarbage = 0, holderYoung = 0;
    {
        RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(holder);
        if (hr != nullptr) {
            holderFree = hr->IsFreeRegion() ? 1u : 0u;
            holderGarbage = hr->IsGarbageRegion() ? 1u : 0u;
            holderYoung = hr->IsYoungRegion() ? 1u : 0u;
        }
    }

    NF2_LOG("INSTALL path=%s kind=%s phase=%u(%s) slot=%p value=%p vkind=%s vbase=%p voff=%zu "
            "holder=%p holderName=%s tipBaseName=%s valYoung=%u valFree=%u valGarbage=%u "
            "holderYoung=%u holderFree=%u holderGarbage=%u ns=%llu "
            "ra0=%p ra1=%p ra2=%p ra3=%p",
            pathStr, kindStr, static_cast<unsigned>(phase), phaseName != nullptr ? phaseName : "?", slot, value,
            KindName(k), reinterpret_cast<void*>(base), offset, reinterpret_cast<void*>(holder), holderName,
            tipBaseName, valYoung, valFree, valGarbage, holderYoung, holderFree, holderGarbage,
            static_cast<unsigned long long>(ns), ra0, ra1, ra2, ra3);

    if (k == Kind::Interior && hb != nullptr) {
        DumpHistBucket(*hb, "on_interior_install");
    }
}

void NextField2Probe::FlushSummary(const char* site)
{
    if (!Enabled()) {
        return;
    }
    uint64_t seen = gSeenInstalls.load(std::memory_order_relaxed);
    uint64_t hits = gNodeHits.load(std::memory_order_relaxed);
    uint64_t base = gBase.load(std::memory_order_relaxed);
    uint64_t interior = gInterior.load(std::memory_order_relaxed);
    uint64_t nulln = gNull.load(std::memory_order_relaxed);
    uint64_t unknown = gUnknown.load(std::memory_order_relaxed);
    uint64_t notHeap = gNotHeap.load(std::memory_order_relaxed);
    uint64_t overflow = gHistOverflow.load(std::memory_order_relaxed);
    TypeInfo* nodeTip = gNodeTip.load(std::memory_order_relaxed);
    NF2_LOG("SUMMARY site=%s seen_installs=%llu node_hits=%llu base=%llu interior=%llu null=%llu "
            "unknown=%llu not_heap=%llu hist_overflow=%llu nodeTip=%p "
            "rej_align=%llu rej_notheap=%llu rej_region=%llu rej_tip=%llu rej_size=%llu "
            "rej_name=%llu cand_tip_valid=%llu coverage=100_on_Node_next posctrl=%s",
            site != nullptr ? site : "?", static_cast<unsigned long long>(seen),
            static_cast<unsigned long long>(hits), static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(interior), static_cast<unsigned long long>(nulln),
            static_cast<unsigned long long>(unknown), static_cast<unsigned long long>(notHeap),
            static_cast<unsigned long long>(overflow), static_cast<void*>(nodeTip),
            static_cast<unsigned long long>(gRejAlign.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(gRejNotHeap.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(gRejRegion.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(gRejTip.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(gRejSize.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(gRejName.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(gCandTipValid.load(std::memory_order_relaxed)),
            (base > 0 && interior == 0)   ? "PASS_all_base"
            : (base > 0 && interior > 0)  ? "PASS_mixed_base_and_interior"
            : (base == 0 && interior > 0) ? "FAIL_no_base"
            : (hits == 0)                 ? "NO_NODE_HITS"
                                          : "OTHER");
    for (int i = 0; i < kMaxPaths; ++i) {
        const char* n = gPaths[i].name.load(std::memory_order_acquire);
        if (n == nullptr) {
            continue;
        }
        uint64_t pt = gPaths[i].total.load(std::memory_order_relaxed);
        uint64_t pb = gPaths[i].base.load(std::memory_order_relaxed);
        uint64_t pi = gPaths[i].interior.load(std::memory_order_relaxed);
        if (pt == 0) {
            continue;
        }
        NF2_LOG("BY_PATH path=%s total=%llu base=%llu interior=%llu", n, static_cast<unsigned long long>(pt),
                static_cast<unsigned long long>(pb), static_cast<unsigned long long>(pi));
    }

    // Dump histories that ever saw interior, or last-kind interior.
    size_t dumped = 0;
    for (size_t i = 0; i < kHistSlots && dumped < 32; ++i) {
        uintptr_t s = gHist[i].slot.load(std::memory_order_relaxed);
        if (s == 0) {
            continue;
        }
        uint64_t li = gHist[i].lastInteriorNs.load(std::memory_order_relaxed);
        uint8_t lk = gHist[i].lastKind.load(std::memory_order_relaxed);
        if (li != 0 || lk == static_cast<uint8_t>(Kind::Interior)) {
            DumpHistBucket(gHist[i], site != nullptr ? site : "flush");
            ++dumped;
        }
    }
}

namespace {
void AtexitFlush()
{
    NextField2Probe::FlushSummary("atexit");
}

struct AtexitRegistrar {
    AtexitRegistrar()
    {
        if (NextField2Probe::Enabled()) {
            std::atexit(AtexitFlush);
        }
    }
};
AtexitRegistrar gAtexitRegistrar;
} // namespace

} // namespace MapleRuntime
