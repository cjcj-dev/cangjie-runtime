// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "InteriorWriterProbe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
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

#define IWR_LOG(fmt, ...)                                                                                              \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][interior-writer] " fmt "\n", ##__VA_ARGS__);                                       \
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

// Same TipLooksValid as InteriorSrcProbe / RemsetHolderProbe (remsetholder POSCTRL 405).
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

// base-first Classify — identical rule to InteriorSrcProbe (interiorfix).
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

thread_local const char* gInstallPath = "sink";

std::atomic<uint64_t> gTotal{0};
std::atomic<uint64_t> gNull{0};
std::atomic<uint64_t> gBase{0};
std::atomic<uint64_t> gInterior{0};
std::atomic<uint64_t> gUnknown{0};
std::atomic<uint64_t> gNotHeap{0};
std::atomic<uint64_t> gSlotAsValue{0}; // value == slot (shape hypothesis)
std::atomic<uint64_t> gDumpLeft{0};
std::atomic<bool> gArmedLogged{false};

// Per-path interior counts (fixed small table).
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

} // namespace

bool InteriorWriterProbe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_INTERIOR_WRITER");
    return on;
}

const char* InteriorWriterProbe::CurrentPath()
{
    return gInstallPath != nullptr ? gInstallPath : "sink";
}

InteriorWriterProbe::ScopedInstallPath::ScopedInstallPath(const char* path)
    : prev_(gInstallPath)
{
    gInstallPath = path != nullptr ? path : "null";
}

InteriorWriterProbe::ScopedInstallPath::~ScopedInstallPath()
{
    gInstallPath = prev_;
}

void InteriorWriterProbe::NoteInstall(const char* path, const char* kind, void* slot, void* value)
{
    if (!Enabled()) {
        return;
    }
    if (!gArmedLogged.exchange(true, std::memory_order_relaxed)) {
        size_t dumpMax = EnvSizeT("MRT_GCV2_INTERIOR_WRITER_DUMP_MAX", 64);
        gDumpLeft.store(dumpMax, std::memory_order_relaxed);
        IWR_LOG("ARMED env=MRT_GCV2_INTERIOR_WRITER=1 dumpMax=%zu", dumpMax);
    }

    const char* pathStr = path;
    if (pathStr == nullptr || pathStr[0] == '\0' || std::strcmp(pathStr, "sink") == 0) {
        pathStr = CurrentPath();
    }
    const char* kindStr = kind != nullptr ? kind : "?";

    uintptr_t val = reinterpret_cast<uintptr_t>(value);
    uintptr_t slotU = reinterpret_cast<uintptr_t>(slot);

    Kind k = Kind::Unknown;
    uintptr_t base = 0;
    size_t offset = 0;
    TypeInfo* tipVal = nullptr;
    TypeInfo* tipBase = nullptr;
    Classify(val, k, base, offset, tipVal, tipBase);

    gTotal.fetch_add(1, std::memory_order_relaxed);
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

    // Shape: installed value equals the slot address (field address as object).
    if (val != 0 && val == slotU) {
        gSlotAsValue.fetch_add(1, std::memory_order_relaxed);
    }

    // Positive control: do not dump normal bases (keep light). Dump interiors always (cap).
    // Also dump slot-as-value and first few unknowns.
    bool dump = false;
    if (k == Kind::Interior) {
        dump = true;
    } else if (val != 0 && val == slotU) {
        dump = true;
    } else if (k == Kind::Unknown && gUnknown.load(std::memory_order_relaxed) <= 8) {
        dump = true;
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

    GCPhase phase = Heap::GetHeap().GetGCPhase();
    const char* phaseName = Collector::GetGCPhaseName(phase);
    void* ra0 = __builtin_return_address(0);
    void* ra1 = __builtin_return_address(1);
    void* ra2 = __builtin_return_address(2);

    const char* tipBaseName = "?";
    if (tipBase != nullptr && TipLooksValid(tipBase)) {
        tipBaseName = tipBase->GetName();
    }

    // slot-as-value or value == some field address shape: value looks like interior of holder of slot
    int slotAsVal = (val != 0 && val == slotU) ? 1 : 0;
    int valueIsSlotOfBase = 0;
    if (k == Kind::Interior && base != 0 && slotU == val) {
        valueIsSlotOfBase = 1;
    }

    IWR_LOG("INSTALL path=%s kind=%s phase=%u(%s) slot=%p value=%p vkind=%s vbase=%p voff=%zu "
            "tipVal=%p tipBase=%p tipBaseName=%s slotAsVal=%d valueEqSlot=%d ra0=%p ra1=%p ra2=%p",
            pathStr, kindStr, static_cast<unsigned>(phase), phaseName != nullptr ? phaseName : "?", slot, value,
            KindName(k), reinterpret_cast<void*>(base), offset, tipVal, tipBase, tipBaseName, slotAsVal,
            valueIsSlotOfBase, ra0, ra1, ra2);
}

void InteriorWriterProbe::FlushSummary(const char* site)
{
    if (!Enabled()) {
        return;
    }
    uint64_t total = gTotal.load(std::memory_order_relaxed);
    uint64_t base = gBase.load(std::memory_order_relaxed);
    uint64_t interior = gInterior.load(std::memory_order_relaxed);
    uint64_t nulln = gNull.load(std::memory_order_relaxed);
    uint64_t unknown = gUnknown.load(std::memory_order_relaxed);
    uint64_t notHeap = gNotHeap.load(std::memory_order_relaxed);
    uint64_t slotAs = gSlotAsValue.load(std::memory_order_relaxed);
    IWR_LOG("SUMMARY site=%s total=%llu base=%llu interior=%llu null=%llu unknown=%llu not_heap=%llu "
            "slotAsValue=%llu posctrl=%s",
            site != nullptr ? site : "?", static_cast<unsigned long long>(total),
            static_cast<unsigned long long>(base), static_cast<unsigned long long>(interior),
            static_cast<unsigned long long>(nulln), static_cast<unsigned long long>(unknown),
            static_cast<unsigned long long>(notHeap), static_cast<unsigned long long>(slotAs),
            (base > 0 && interior == 0)     ? "PASS_all_base"
            : (base > 0 && interior > 0)    ? "PASS_mixed_base_and_interior"
            : (base == 0 && interior > 0)   ? "FAIL_no_base"
            : (base == 0 && total == 0)     ? "NO_SAMPLES"
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
        IWR_LOG("BY_PATH path=%s total=%llu base=%llu interior=%llu", n, static_cast<unsigned long long>(pt),
                static_cast<unsigned long long>(pb), static_cast<unsigned long long>(pi));
    }
}

} // namespace MapleRuntime
