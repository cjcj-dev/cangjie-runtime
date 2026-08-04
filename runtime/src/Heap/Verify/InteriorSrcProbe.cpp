// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "InteriorSrcProbe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

// Conservative "looks like a real TypeInfo pointer" — mirrors VerifyHeap H2 online checks
// without calling into fail-closed product CHECKs.
bool TipLooksValid(TypeInfo* tip)
{
    if (tip == nullptr) {
        return false;
    }
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return false;
    }
    // TypeInfo must not live in the managed heap (defect D / code-as-TI both fail this).
    if (Heap::IsHeapAddress(tipAddr)) {
        return false;
    }
    // IsVaildType only reads the type-kind byte; safe on any mapped TypeInfo / .cjmetadata.
    // On FILE_RX code it may spuriously pass or fail — combined with !IsHeapAddress this is
    // still useful for AutoEnv B (metadata) vs B+16 (code entry) discrimination.
    return tip->IsVaildType();
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

// Prefer explicit interior when value-N has a valid TypeInfo and value itself does not;
// if both look valid, prefer base (offset 0). Walk object stream only when tip at value
// is invalid — GetAllocSize on a code-as-TI interior would return a giant garbage size.
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

    // Interior candidates: value-8 / value-16 / value-24 (AutoEnv slots + one more).
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
        // Bounds: if GetSize is sane, require value ∈ [cand, cand+size).
        // Cap size to region to avoid following garbage instanceSize.
        size_t size = 0;
        bool sizeOk = false;
        {
            auto* baseObj = reinterpret_cast<BaseObject*>(cand);
            // Only call GetSize when tip looks valid (not code-as-TI).
            size = baseObj->GetSize();
            MAddress rStart = region->GetRegionStart();
            MAddress rEnd = region->GetRegionEnd();
            if (size >= 8 && size <= (rEnd - rStart) && cand + size <= rEnd && value >= cand && value < cand + size) {
                sizeOk = true;
            }
        }
        if (!sizeOk && !valueBaseLike) {
            // Still accept when value itself is NOT base-like (classic B+16 code-as-TI case):
            // tip at value is code/invalid; tip at cand is metadata.
            sizeOk = true;
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

    if (valueBaseLike) {
        kind = Kind::Base;
        baseOut = value;
        offsetOut = 0;
        tipAtBase = tipAtValue;
        return;
    }
    kind = Kind::Unknown;
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
    switch (kind) {
        case Kind::Base:
            gBase.fetch_add(1, std::memory_order_relaxed);
            gSrcBase[sidx].fetch_add(1, std::memory_order_relaxed);
            break;
        case Kind::Interior:
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
            break;
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

    GCPhase phase = Heap::GetHeap().GetGCPhase();
    const char* phaseName = Collector::GetGCPhaseName(phase);
    const char* srcStr = source != nullptr ? source : "null";
    ISRC_LOG("PUSH source=%s kind=%s value=%p base=%p offset=%zu tipVal=%p tipBase=%p "
             "slot=%#zx slotVal=%#zx phase=%s(%u) role=%s",
             srcStr, KindName(kind), object, reinterpret_cast<void*>(base), offset,
             static_cast<void*>(tipVal), static_cast<void*>(tipBase), static_cast<size_t>(slot),
             static_cast<size_t>(slotVal != 0 ? slotVal : value), phaseName, static_cast<unsigned>(phase), RoleName());
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
}

} // namespace MapleRuntime
