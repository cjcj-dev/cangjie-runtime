// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "InteriorSrcProbe.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Common/StateWord.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/MClass.inline.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {

#define ISRC_LOG(fmt, ...)                                                                                             \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][interior-src] " fmt "\n", ##__VA_ARGS__);                                          \
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

// True if the page containing addr is mapped (mincore). Never dereferences unmapped memory.
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
    // mincore returns 0 if the range is mapped (resident bit in vec is separate).
    if (mincore(reinterpret_cast<void*>(page), static_cast<size_t>(pageSize), &vec) == 0) {
        return true;
    }
    // ENOMEM/EINVAL ⇒ not mapped in this process.
    return false;
}

// Read TypeInfo pointer the same way StateWord does, without requiring a live BaseObject.
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

// Crash-safe TypeInfo probe: never touch unmapped tip pages; reject code-as-TI via size cap.
bool TipLooksValid(TypeInfo* tip)
{
    if (tip == nullptr) {
        return false;
    }
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return false;
    }
    // TypeInfo must not live in the managed heap (defect D).
    if (Heap::IsHeapAddress(tipAddr)) {
        return false;
    }
    // Generic TI mmap range is authoritative when it hits.
    bool inTim = TypeInfoManager::GetTypeInfoManager().ContainsAddress(tipAddr);
    if (!inTim && !PageMapped(tipAddr)) {
        return false;
    }
    // Safe to read type-kind / instanceSize only after page check (static .cjmetadata or TIM).
    if (!tip->IsVaildType()) {
        return false;
    }
    // Code-as-TI yields huge garbage instanceSize (magicaddr: ~1.2e9). Cap at 1MiB payload.
    MSize isz = tip->GetInstanceSize();
    if (isz == 0 || isz > (1u << 20)) {
        return false;
    }
    return true;
}

// Non-array object size from tip only (no BaseObject::GetSize — that path can be heavy/unsafe).
size_t SaneObjectSize(TypeInfo* tip, RegionInfo* region)
{
    if (tip == nullptr || region == nullptr) {
        return 0;
    }
    MSize isz = tip->GetInstanceSize();
    // AlignUp(instanceSize + TYPEINFO_PTR_SIZE, 8) for non-array path (BaseObject::GetSize).
    size_t size = (static_cast<size_t>(isz) + 8u + 7u) & ~static_cast<size_t>(7u);
    size_t regionBytes = region->GetRegionEnd() - region->GetRegionStart();
    if (size < 16 || size > regionBytes || size > (1u << 20)) {
        return 0;
    }
    return size;
}

enum class Kind : uint8_t { Base = 0, Interior = 1, Unknown = 2, NotHeap = 3 };

const char* KindName(Kind k)
{
    switch (k) {
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

// Source bucket indices for atomic counters.
enum SrcIdx : int {
    SRC_ALLOC_BUFFER = 0,
    SRC_MINOR_ROOT = 1,
    SRC_CLOSURE_EDGE = 2,
    SRC_REMSET = 3,
    SRC_OTHER = 4,
    SRC_N = 5
};

int SourceIndex(const char* source)
{
    if (source == nullptr) {
        return SRC_OTHER;
    }
    if (std::strcmp(source, "alloc_buffer") == 0) {
        return SRC_ALLOC_BUFFER;
    }
    if (std::strcmp(source, "minor_root") == 0 || std::strncmp(source, "mutator_", 8) == 0 ||
        std::strcmp(source, "static") == 0 || std::strcmp(source, "concurrency") == 0 ||
        std::strcmp(source, "finalizer") == 0 || std::strcmp(source, "export") == 0 ||
        std::strncmp(source, "value_", 6) == 0) {
        // VisitMinorRootSlots tags TLS as mutator_stack/static/...; funnel still says minor_root.
        return SRC_MINOR_ROOT;
    }
    if (std::strcmp(source, "closure_edge") == 0) {
        return SRC_CLOSURE_EDGE;
    }
    if (std::strcmp(source, "remset") == 0) {
        return SRC_REMSET;
    }
    return SRC_OTHER;
}

const char* SourceName(int idx)
{
    switch (idx) {
        case SRC_ALLOC_BUFFER:
            return "alloc_buffer";
        case SRC_MINOR_ROOT:
            return "minor_root";
        case SRC_CLOSURE_EDGE:
            return "closure_edge";
        case SRC_REMSET:
            return "remset";
        default:
            return "other";
    }
}

std::atomic<uint64_t> gTotal{0};
std::atomic<uint64_t> gBase{0};
std::atomic<uint64_t> gInterior{0};
std::atomic<uint64_t> gUnknown{0};
std::atomic<uint64_t> gNotHeap{0};
std::atomic<uint64_t> gSrcTotal[SRC_N]{};
std::atomic<uint64_t> gSrcInterior[SRC_N]{};
std::atomic<uint64_t> gSrcBase[SRC_N]{};
std::atomic<uint64_t> gInteriorOff8{0};
std::atomic<uint64_t> gInteriorOff16{0};
std::atomic<uint64_t> gInteriorOff24{0};
std::atomic<uint64_t> gInteriorOffOther{0};
std::atomic<uint64_t> gDumpLeft{0};
std::atomic<bool> gArmedLogged{false};

// clsaudit T0: known-base positive control (value tip legal ⇒ must never be interior).
std::atomic<uint64_t> gPosCtrlKnownBase{0};
std::atomic<uint64_t> gPosCtrlFalseInterior{0};
std::atomic<uint64_t> gPosCtrlAllocBase{0};
std::atomic<uint64_t> gPosCtrlAllocInterior{0};
// Adjacent-tip geometry (old FP fuel): value is base AND some value-k also tip-valid.
std::atomic<uint64_t> gBaseAlsoAdjTip{0};

// clsaudit T1: for each interior, how many of {8,16,24,32} candidates look legal.
std::atomic<uint64_t> gMultiHit0{0};
std::atomic<uint64_t> gMultiHit1{0};
std::atomic<uint64_t> gMultiHit2{0};
std::atomic<uint64_t> gMultiHit3{0};
std::atomic<uint64_t> gMultiHit4{0};
std::atomic<uint64_t> gHitOff8{0};
std::atomic<uint64_t> gHitOff16{0};
std::atomic<uint64_t> gHitOff24{0};
std::atomic<uint64_t> gHitOff32{0};
// First-hit vs sole-hit at 16.
std::atomic<uint64_t> gInteriorFirst16Sole{0};
std::atomic<uint64_t> gInteriorFirst16Multi{0};

// clsaudit T2: gold dual check tipVal≈$i code · tipBase name has AutoEnv.
std::atomic<uint64_t> gGoldDual{0};
std::atomic<uint64_t> gGoldTipBaseAutoEnv{0};
std::atomic<uint64_t> gGoldTipValCode{0};
std::atomic<uint64_t> gGoldTipValSymI{0};

// T3: last true-interior enqueue (overwritten on each interior NotePush).
std::mutex gLastMu;
char gLastSrc[64] = {};
uintptr_t gLastValue = 0;
uintptr_t gLastBase = 0;
uintptr_t gLastSlot = 0;
size_t gLastOffset = 0;
unsigned gLastPhase = 0;
char gLastPhaseName[48] = {};
uint64_t gLastSeq = 0;

void RememberLastInterior(const char* source, uintptr_t value, uintptr_t base, size_t offset, uintptr_t slot,
                          GCPhase phase, const char* phaseName)
{
    std::lock_guard<std::mutex> lock(gLastMu);
    gLastSeq += 1;
    gLastValue = value;
    gLastBase = base;
    gLastOffset = offset;
    gLastSlot = slot;
    gLastPhase = static_cast<unsigned>(phase);
    std::snprintf(gLastSrc, sizeof(gLastSrc), "%s", source != nullptr ? source : "null");
    std::snprintf(gLastPhaseName, sizeof(gLastPhaseName), "%s", phaseName != nullptr ? phaseName : "?");
}

// Same legality test Classify uses for a candidate base = value - off (no early return).
bool CandLooksLegal(uintptr_t value, size_t off, RegionInfo* region, TypeInfo*& tipOut)
{
    tipOut = nullptr;
    if (region == nullptr || value < off) {
        return false;
    }
    uintptr_t cand = value - off;
    if (!Heap::IsHeapAddress(cand)) {
        return false;
    }
    RegionInfo* candRegion = RegionInfo::TryGetRegionInfoAt(cand);
    if (candRegion != region) {
        return false;
    }
    TypeInfo* tip = PeekTypeInfoAt(cand);
    if (!TipLooksValid(tip)) {
        return false;
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
        return false;
    }
    tipOut = tip;
    return true;
}

// Scan all four offsets; return hit count and first-hit offset/base/tip (8→16→24→32 order).
int ScanAllOffsets(uintptr_t value, RegionInfo* region, size_t& firstOff, uintptr_t& firstBase, TypeInfo*& firstTip,
                   bool hitOff[4])
{
    static const size_t kOffs[] = {8, 16, 24, 32};
    int n = 0;
    firstOff = 0;
    firstBase = 0;
    firstTip = nullptr;
    for (int i = 0; i < 4; ++i) {
        hitOff[i] = false;
        TypeInfo* tip = nullptr;
        if (!CandLooksLegal(value, kOffs[i], region, tip)) {
            continue;
        }
        hitOff[i] = true;
        ++n;
        if (firstOff == 0) {
            firstOff = kOffs[i];
            firstBase = value - kOffs[i];
            firstTip = tip;
        }
    }
    return n;
}

// interiorfix rule: if value itself is a legal object base, ALWAYS classify Base.
// Never reclassify as interior because value-k has a valid tip (adjacent-object FP:
// e.g. OutOfMemoryError base with RawArray tip at value-16). Only when value is NOT a
// legal base, look for interior at value-k (k in {8,16,24,32}).
void Classify(uintptr_t value, Kind& kind, uintptr_t& baseOut, size_t& offsetOut, TypeInfo*& tipAtValue,
              TypeInfo*& tipAtBase)
{
    kind = Kind::Unknown;
    baseOut = 0;
    offsetOut = 0;
    tipAtValue = nullptr;
    tipAtBase = nullptr;

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
    bool valueBaseLike = TipLooksValid(tipAtValue);

    // Gold: legal tip at value ⇒ base. Bidirectional: known base (OOM static) must hit here.
    if (valueBaseLike) {
        kind = Kind::Base;
        baseOut = value;
        offsetOut = 0;
        tipAtBase = tipAtValue;
        return;
    }

    // Gold: only non-base values may be interior (AutoEnv B+16: tip at value is code).
    // First-hit order preserved (8→16→24→32); T1 multi-hit scanned separately.
    bool hitOff[4] = {};
    size_t firstOff = 0;
    uintptr_t firstBase = 0;
    TypeInfo* firstTip = nullptr;
    int n = ScanAllOffsets(value, region, firstOff, firstBase, firstTip, hitOff);
    if (n > 0) {
        kind = Kind::Interior;
        baseOut = firstBase;
        offsetOut = firstOff;
        tipAtBase = firstTip;
        return;
    }

    kind = Kind::Unknown;
}

// Safe short name for tip (empty if unreadable).
const char* SafeTipName(TypeInfo* tip)
{
    if (tip == nullptr || !TipLooksValid(tip)) {
        return "";
    }
    const char* n = tip->GetName();
    return n != nullptr ? n : "";
}

bool NameHasAutoEnv(const char* name)
{
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    // Literal AutoEnv (if demangled) OR Cangjie closure-env TypeInfo mangling:
    // e.g. default:$ClN7default4mainHvEL1_E$0 (staticinterior/b4abi gold tipBase).
    if (std::strstr(name, "AutoEnv") != nullptr || std::strstr(name, "autoenv") != nullptr) {
        return true;
    }
    if (std::strstr(name, "$Cl") != nullptr || std::strstr(name, ":$Cl") != nullptr) {
        return true;
    }
    // Fallback: "$ClN" / "ClN" closure class forms.
    if (std::strstr(name, "ClN") != nullptr && std::strstr(name, "$") != nullptr) {
        return true;
    }
    return false;
}

// tipVal looks like code entry (not a valid TypeInfo) — gold half for $i.
bool TipValLooksLikeCode(TypeInfo* tipVal, uintptr_t value)
{
    if (TipLooksValid(tipVal)) {
        return false;
    }
    // Peek may yield a non-null garbage "tip" (code bytes as pointer). Require mapped page.
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tipVal);
    if (tipAddr == 0) {
        // No tip word — still may be interior into non-header; not gold $i shape.
        return false;
    }
    if (Heap::IsHeapAddress(tipAddr)) {
        return false;
    }
    if (!PageMapped(tipAddr)) {
        return false;
    }
    // Prefer executable mapping for true code.
    Dl_info dli;
    if (dladdr(reinterpret_cast<void*>(tipAddr), &dli) != 0 && dli.dli_sname != nullptr) {
        return true;
    }
    // Mapped non-heap tip that failed TipLooksValid (code-as-TI / huge isz) counts as code-like.
    (void)value;
    return true;
}

bool SymLooksLikeDollarI(TypeInfo* tipVal)
{
    if (tipVal == nullptr) {
        return false;
    }
    Dl_info dli;
    if (dladdr(reinterpret_cast<void*>(tipVal), &dli) == 0 || dli.dli_sname == nullptr) {
        return false;
    }
    // Cangjie lambda/code symbols often contain "$i" or end with patterns from closure.
    return std::strstr(dli.dli_sname, "$i") != nullptr || std::strstr(dli.dli_sname, "$I") != nullptr;
}

const char* RoleName()
{
    if (IsGcThread()) {
        return "gc";
    }
    if (IsRuntimeThread()) {
        return "runtime";
    }
    return "mutator";
}

} // namespace

bool InteriorSrcProbe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_INTERIOR_SRC");
    return on;
}

void InteriorSrcProbe::NotePush(const char* source, void* object, uintptr_t slot, uintptr_t slotVal)
{
    if (!Enabled()) {
        return;
    }
    if (!gArmedLogged.exchange(true, std::memory_order_relaxed)) {
        size_t dumpMax = EnvSizeT("MRT_GCV2_INTERIOR_SRC_DUMP_MAX", 64);
        gDumpLeft.store(dumpMax, std::memory_order_relaxed);
        ISRC_LOG("ARMED env=MRT_GCV2_INTERIOR_SRC=1 dumpMax=%zu", dumpMax);
    }

    uintptr_t value = reinterpret_cast<uintptr_t>(object);
    Kind kind = Kind::Unknown;
    uintptr_t base = 0;
    size_t offset = 0;
    TypeInfo* tipVal = nullptr;
    TypeInfo* tipBase = nullptr;
    Classify(value, kind, base, offset, tipVal, tipBase);

    int sidx = SourceIndex(source);
    gTotal.fetch_add(1, std::memory_order_relaxed);
    gSrcTotal[sidx].fetch_add(1, std::memory_order_relaxed);
    GCPhase phase = Heap::GetHeap().GetGCPhase();
    const char* phaseName = Collector::GetGCPhaseName(phase);
    const char* srcStr = source != nullptr ? source : "null";

    // Multi-hit scan (T1) + adjacent-tip geometry on known bases (T0 fuel).
    int multiHits = 0;
    bool hitOff[4] = {};
    size_t firstOffScan = 0;
    uintptr_t firstBaseScan = 0;
    TypeInfo* firstTipScan = nullptr;
    RegionInfo* regionForScan = Heap::IsHeapAddress(value) ? RegionInfo::TryGetRegionInfoAt(value) : nullptr;
    if (regionForScan != nullptr && !regionForScan->IsFreeRegion() && !regionForScan->IsGarbageRegion()) {
        multiHits = ScanAllOffsets(value, regionForScan, firstOffScan, firstBaseScan, firstTipScan, hitOff);
    }

    switch (kind) {
        case Kind::Base: {
            gBase.fetch_add(1, std::memory_order_relaxed);
            gSrcBase[sidx].fetch_add(1, std::memory_order_relaxed);
            // T0 positive control: tip-legal value MUST classify as base (already here).
            gPosCtrlKnownBase.fetch_add(1, std::memory_order_relaxed);
            if (sidx == SRC_ALLOC_BUFFER) {
                gPosCtrlAllocBase.fetch_add(1, std::memory_order_relaxed);
            }
            // Old FP fuel: if base also has adjacent tip-legal candidates, record geometry.
            if (multiHits > 0) {
                gBaseAlsoAdjTip.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        case Kind::Interior: {
            gInterior.fetch_add(1, std::memory_order_relaxed);
            gSrcInterior[sidx].fetch_add(1, std::memory_order_relaxed);
            if (offset == 8) {
                gInteriorOff8.fetch_add(1, std::memory_order_relaxed);
            } else if (offset == 16) {
                gInteriorOff16.fetch_add(1, std::memory_order_relaxed);
            } else if (offset == 24) {
                gInteriorOff24.fetch_add(1, std::memory_order_relaxed);
            } else {
                gInteriorOffOther.fetch_add(1, std::memory_order_relaxed);
            }
            // T0 false-interior: should never fire if tip at value is legal (Classify returns Base).
            // Still count if tipVal somehow looks base-like under TipLooksValid (defensive).
            if (TipLooksValid(tipVal)) {
                gPosCtrlFalseInterior.fetch_add(1, std::memory_order_relaxed);
                if (sidx == SRC_ALLOC_BUFFER) {
                    gPosCtrlAllocInterior.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // T1 multi-hit distribution among interiors.
            if (multiHits <= 0) {
                gMultiHit0.fetch_add(1, std::memory_order_relaxed);
            } else if (multiHits == 1) {
                gMultiHit1.fetch_add(1, std::memory_order_relaxed);
            } else if (multiHits == 2) {
                gMultiHit2.fetch_add(1, std::memory_order_relaxed);
            } else if (multiHits == 3) {
                gMultiHit3.fetch_add(1, std::memory_order_relaxed);
            } else {
                gMultiHit4.fetch_add(1, std::memory_order_relaxed);
            }
            if (hitOff[0]) {
                gHitOff8.fetch_add(1, std::memory_order_relaxed);
            }
            if (hitOff[1]) {
                gHitOff16.fetch_add(1, std::memory_order_relaxed);
            }
            if (hitOff[2]) {
                gHitOff24.fetch_add(1, std::memory_order_relaxed);
            }
            if (hitOff[3]) {
                gHitOff32.fetch_add(1, std::memory_order_relaxed);
            }
            if (offset == 16) {
                if (multiHits == 1) {
                    gInteriorFirst16Sole.fetch_add(1, std::memory_order_relaxed);
                } else if (multiHits > 1) {
                    gInteriorFirst16Multi.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // T2 gold dual: tipBase name has AutoEnv AND tipVal looks like code ($i).
            const char* baseName = SafeTipName(tipBase);
            bool goldBase = NameHasAutoEnv(baseName);
            bool goldCode = TipValLooksLikeCode(tipVal, value);
            bool goldSym = SymLooksLikeDollarI(tipVal);
            if (goldBase) {
                gGoldTipBaseAutoEnv.fetch_add(1, std::memory_order_relaxed);
            }
            if (goldCode) {
                gGoldTipValCode.fetch_add(1, std::memory_order_relaxed);
            }
            if (goldSym) {
                gGoldTipValSymI.fetch_add(1, std::memory_order_relaxed);
            }
            // Dual: AutoEnv base tip + code-like tipVal at value (staticinterior gold).
            if (goldBase && goldCode) {
                gGoldDual.fetch_add(1, std::memory_order_relaxed);
            }
            RememberLastInterior(srcStr, value, base, offset, slot, phase, phaseName);
            break;
        }
        case Kind::NotHeap:
            gNotHeap.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            gUnknown.fetch_add(1, std::memory_order_relaxed);
            break;
    }

    // Always dump interiors; sample bases for positive control; dump unknowns sparingly.
    bool dump = false;
    if (kind == Kind::Interior) {
        dump = true;
    } else if (kind == Kind::Base) {
        uint64_t n = gBase.load(std::memory_order_relaxed);
        dump = (n <= 4) || ((n & 0xffff) == 1); // first few + sparse sample
    } else if (kind == Kind::Unknown) {
        dump = gUnknown.load(std::memory_order_relaxed) <= 16;
    }

    if (!dump) {
        return;
    }
    uint64_t left = gDumpLeft.load(std::memory_order_relaxed);
    if (left == 0) {
        return;
    }
    if (!gDumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
        return;
    }

    const char* tipBaseName = SafeTipName(tipBase);
    const char* tipValSym = "";
    char tipValSymBuf[128] = {};
    if (tipVal != nullptr) {
        Dl_info dli;
        if (dladdr(reinterpret_cast<void*>(tipVal), &dli) != 0 && dli.dli_sname != nullptr) {
            std::snprintf(tipValSymBuf, sizeof(tipValSymBuf), "%s", dli.dli_sname);
            tipValSym = tipValSymBuf;
        }
    }
    ISRC_LOG("PUSH source=%s kind=%s value=%p base=%p offset=%zu tipVal=%p tipBase=%p "
             "slot=%#zx slotVal=%#zx phase=%s(%u) role=%s multiHits=%d hits=%d%d%d%d "
             "tipBaseName=%s tipValSym=%s",
             srcStr, KindName(kind), object, reinterpret_cast<void*>(base), offset,
             static_cast<void*>(tipVal), static_cast<void*>(tipBase), static_cast<size_t>(slot),
             static_cast<size_t>(slotVal != 0 ? slotVal : value), phaseName, static_cast<unsigned>(phase), RoleName(),
             multiHits, hitOff[0] ? 1 : 0, hitOff[1] ? 1 : 0, hitOff[2] ? 1 : 0, hitOff[3] ? 1 : 0,
             tipBaseName != nullptr ? tipBaseName : "", tipValSym);
}

void InteriorSrcProbe::FlushSummary(const char* site)
{
    if (!Enabled()) {
        return;
    }
    uint64_t total = gTotal.load(std::memory_order_relaxed);
    uint64_t base = gBase.load(std::memory_order_relaxed);
    uint64_t interior = gInterior.load(std::memory_order_relaxed);
    uint64_t unknown = gUnknown.load(std::memory_order_relaxed);
    uint64_t notHeap = gNotHeap.load(std::memory_order_relaxed);
    ISRC_LOG("SUMMARY site=%s total=%llu base=%llu interior=%llu unknown=%llu not_heap=%llu "
             "int_off8=%llu int_off16=%llu int_off24=%llu int_offOther=%llu",
             site != nullptr ? site : "?", static_cast<unsigned long long>(total),
             static_cast<unsigned long long>(base), static_cast<unsigned long long>(interior),
             static_cast<unsigned long long>(unknown), static_cast<unsigned long long>(notHeap),
             static_cast<unsigned long long>(gInteriorOff8.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(gInteriorOff16.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(gInteriorOff24.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(gInteriorOffOther.load(std::memory_order_relaxed)));
    // clsaudit T0/T1/T2 machine lines (parse-friendly).
    uint64_t pcKnown = gPosCtrlKnownBase.load(std::memory_order_relaxed);
    uint64_t pcFalse = gPosCtrlFalseInterior.load(std::memory_order_relaxed);
    uint64_t pcAllocB = gPosCtrlAllocBase.load(std::memory_order_relaxed);
    uint64_t pcAllocI = gPosCtrlAllocInterior.load(std::memory_order_relaxed);
    uint64_t baseAdj = gBaseAlsoAdjTip.load(std::memory_order_relaxed);
    ISRC_LOG("CLSA_T0 site=%s known_base=%llu false_interior=%llu alloc_base=%llu alloc_interior=%llu "
             "base_also_adj_tip=%llu CLSA_FALSE_INTERIOR_%llu/%llu",
             site != nullptr ? site : "?", static_cast<unsigned long long>(pcKnown),
             static_cast<unsigned long long>(pcFalse), static_cast<unsigned long long>(pcAllocB),
             static_cast<unsigned long long>(pcAllocI), static_cast<unsigned long long>(baseAdj),
             static_cast<unsigned long long>(pcFalse), static_cast<unsigned long long>(pcKnown));
    uint64_t mh0 = gMultiHit0.load(std::memory_order_relaxed);
    uint64_t mh1 = gMultiHit1.load(std::memory_order_relaxed);
    uint64_t mh2 = gMultiHit2.load(std::memory_order_relaxed);
    uint64_t mh3 = gMultiHit3.load(std::memory_order_relaxed);
    uint64_t mh4 = gMultiHit4.load(std::memory_order_relaxed);
    uint64_t sole16 = gInteriorFirst16Sole.load(std::memory_order_relaxed);
    uint64_t multi16 = gInteriorFirst16Multi.load(std::memory_order_relaxed);
    ISRC_LOG("CLSA_T1 site=%s multiHit_0=%llu multiHit_1=%llu multiHit_2=%llu multiHit_3=%llu multiHit_4=%llu "
             "cand_hit_off8=%llu cand_hit_off16=%llu cand_hit_off24=%llu cand_hit_off32=%llu "
             "first16_sole=%llu first16_multi=%llu CLSA_MULTIHIT_%llu:%llu:%llu:%llu:%llu",
             site != nullptr ? site : "?", static_cast<unsigned long long>(mh0),
             static_cast<unsigned long long>(mh1), static_cast<unsigned long long>(mh2),
             static_cast<unsigned long long>(mh3), static_cast<unsigned long long>(mh4),
             static_cast<unsigned long long>(gHitOff8.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(gHitOff16.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(gHitOff24.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(gHitOff32.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(sole16), static_cast<unsigned long long>(multi16),
             static_cast<unsigned long long>(mh0), static_cast<unsigned long long>(mh1),
             static_cast<unsigned long long>(mh2), static_cast<unsigned long long>(mh3),
             static_cast<unsigned long long>(mh4));
    uint64_t gold = gGoldDual.load(std::memory_order_relaxed);
    ISRC_LOG("CLSA_T2 site=%s gold_dual=%llu tipBase_AutoEnv=%llu tipVal_code=%llu tipVal_sym_$i=%llu "
             "interior_total=%llu CLSA_GOLD_%llu/%llu",
             site != nullptr ? site : "?", static_cast<unsigned long long>(gold),
             static_cast<unsigned long long>(gGoldTipBaseAutoEnv.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(gGoldTipValCode.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(gGoldTipValSymI.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(interior), static_cast<unsigned long long>(gold),
             static_cast<unsigned long long>(interior));
    for (int i = 0; i < SRC_N; ++i) {
        uint64_t t = gSrcTotal[i].load(std::memory_order_relaxed);
        uint64_t b = gSrcBase[i].load(std::memory_order_relaxed);
        uint64_t n = gSrcInterior[i].load(std::memory_order_relaxed);
        if (t == 0 && b == 0 && n == 0) {
            continue;
        }
        ISRC_LOG("BY_SOURCE source=%s total=%llu base=%llu interior=%llu", SourceName(i),
                 static_cast<unsigned long long>(t), static_cast<unsigned long long>(b),
                 static_cast<unsigned long long>(n));
    }
    // T3: last interior enqueue before this flush (or process death if printed from invalid path).
    {
        std::lock_guard<std::mutex> lock(gLastMu);
        if (gLastSeq == 0) {
            ISRC_LOG("LAST_INTERIOR site=%s seq=0 (none)", site != nullptr ? site : "?");
        } else {
            ISRC_LOG("LAST_INTERIOR site=%s seq=%llu source=%s value=%#zx base=%#zx offset=%zu "
                     "slot=%#zx phase=%s(%u)",
                     site != nullptr ? site : "?", static_cast<unsigned long long>(gLastSeq), gLastSrc,
                     static_cast<size_t>(gLastValue), static_cast<size_t>(gLastBase), gLastOffset,
                     static_cast<size_t>(gLastSlot), gLastPhaseName, gLastPhase);
        }
    }
}

} // namespace MapleRuntime
