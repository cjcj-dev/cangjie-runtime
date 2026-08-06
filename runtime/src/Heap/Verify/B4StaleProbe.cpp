// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "B4StaleProbe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/BaseObject.h"
#include "Common/StateWord.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/MClass.inline.h"
#include "ObjectModel/RefField.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {

#define B4S_LOG(fmt, ...)                                                                                              \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][b4stale] " fmt "\n", ##__VA_ARGS__);                                                \
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
    MSize isz = tip->GetInstanceSize();
    size_t size = (static_cast<size_t>(isz) + 8u + 7u) & ~static_cast<size_t>(7u);
    size_t regionBytes = region->GetRegionEnd() - region->GetRegionStart();
    if (size < 16 || size > regionBytes || size > (1u << 20)) {
        return 0;
    }
    return size;
}

enum class Kind : uint8_t { Base = 0, Interior = 1, Unknown = 2, NotHeap = 3, Null = 4 };

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
        case Kind::Null:
            return "null";
        default:
            return "?";
    }
}

// base-first classifier (staticinterior / b4copy gold rule).
void Classify(uintptr_t value, Kind& kind, uintptr_t& baseOut, size_t& offsetOut, TypeInfo*& tipAtBase)
{
    kind = Kind::Unknown;
    baseOut = 0;
    offsetOut = 0;
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

    TypeInfo* tipAtValue = PeekTypeInfoAt(value);
    if (TipLooksValid(tipAtValue)) {
        kind = Kind::Base;
        baseOut = value;
        offsetOut = 0;
        tipAtBase = tipAtValue;
        return;
    }

    static const size_t kOffs[] = { 8, 16, 24, 32, 40, 48, 56, 64 };
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
}

const char* TipName(TypeInfo* tip)
{
    if (tip == nullptr || !TipLooksValid(tip)) {
        return "?";
    }
    const char* n = tip->GetName();
    return n != nullptr ? n : "?";
}

std::mutex gMu;
std::atomic<bool> gArmed{ false };
std::atomic<uint64_t> gDumpLeft{ 0 };

// Rolling base snapshots: last two STW base sets (pre/post-evac of recent minors).
// unionPrior = all bases ever seen in any completed snapshot (for NEVER_VALID).
std::unordered_set<uintptr_t> gSnapA;
std::unordered_set<uintptr_t> gSnapB;
std::unordered_set<uintptr_t> gUnionPrior;
const char* gSnapAPoint = "";
const char* gSnapBPoint = "";
uint64_t gSnapAGen = 0;
uint64_t gSnapBGen = 0;
uint64_t gSnapGen = 0;
bool gUseAAsLatest = true;

// Per-base size recorded at last snapshot (for T1 layout correlation).
std::unordered_map<uintptr_t, uint32_t> gPriorBaseSize;

std::atomic<uint64_t> gSnapCalls{ 0 };
std::atomic<uint64_t> gSnapBases{ 0 };
std::atomic<uint64_t> gScanCalls{ 0 };
std::atomic<uint64_t> gSlots{ 0 };
std::atomic<uint64_t> gInterior{ 0 };
std::atomic<uint64_t> gStaleConfirmed{ 0 };
std::atomic<uint64_t> gNeverValid{ 0 };
std::atomic<uint64_t> gStillBaseNow{ 0 }; // classified interior but value also in current bases (classifier noise)
std::atomic<uint64_t> gInLatestSnap{ 0 };
std::atomic<uint64_t> gInPriorOnly{ 0 };
std::atomic<uint64_t> gHostNotInPrior{ 0 }; // host base N is newly allocated after prior snap
std::atomic<uint64_t> gHostInPrior{ 0 };

// offset histogram buckets: 8,16,24,32,40,48,56,64,other
std::atomic<uint64_t> gOff8{ 0 };
std::atomic<uint64_t> gOff16{ 0 };
std::atomic<uint64_t> gOff24{ 0 };
std::atomic<uint64_t> gOff32{ 0 };
std::atomic<uint64_t> gOff40{ 0 };
std::atomic<uint64_t> gOff48{ 0 };
std::atomic<uint64_t> gOffOther{ 0 };

// host size histogram (rounded): 16,24,32,40,48,64,96,128,256,big
std::atomic<uint64_t> gHostSz16{ 0 };
std::atomic<uint64_t> gHostSz24{ 0 };
std::atomic<uint64_t> gHostSz32{ 0 };
std::atomic<uint64_t> gHostSz48{ 0 };
std::atomic<uint64_t> gHostSz64{ 0 };
std::atomic<uint64_t> gHostSz96{ 0 };
std::atomic<uint64_t> gHostSz128{ 0 };
std::atomic<uint64_t> gHostSzOther{ 0 };

// unique interior values classified (bounded set for dump uniqueness)
std::unordered_set<uintptr_t> gSeenInteriorVals;
std::unordered_set<uintptr_t> gSeenStaleVals;
std::unordered_set<uintptr_t> gSeenNeverVals;

bool TryTakeDump()
{
    uint64_t left = gDumpLeft.load(std::memory_order_relaxed);
    while (left > 0) {
        if (gDumpLeft.compare_exchange_weak(left, left - 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void ArmOnce()
{
    if (gArmed.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    size_t dumpMax = EnvSizeT("MRT_GCV2_B4STALE_DUMP_MAX", 64);
    gDumpLeft.store(dumpMax, std::memory_order_relaxed);
    B4S_LOG("ARMED env=MRT_GCV2_B4STALE=1 dumpMax=%zu", dumpMax);
}

void CountOffset(size_t off)
{
    if (off == 8) {
        gOff8.fetch_add(1, std::memory_order_relaxed);
    } else if (off == 16) {
        gOff16.fetch_add(1, std::memory_order_relaxed);
    } else if (off == 24) {
        gOff24.fetch_add(1, std::memory_order_relaxed);
    } else if (off == 32) {
        gOff32.fetch_add(1, std::memory_order_relaxed);
    } else if (off == 40) {
        gOff40.fetch_add(1, std::memory_order_relaxed);
    } else if (off == 48) {
        gOff48.fetch_add(1, std::memory_order_relaxed);
    } else {
        gOffOther.fetch_add(1, std::memory_order_relaxed);
    }
}

void CountHostSize(size_t sz)
{
    if (sz == 16) {
        gHostSz16.fetch_add(1, std::memory_order_relaxed);
    } else if (sz == 24) {
        gHostSz24.fetch_add(1, std::memory_order_relaxed);
    } else if (sz == 32) {
        gHostSz32.fetch_add(1, std::memory_order_relaxed);
    } else if (sz == 48) {
        gHostSz48.fetch_add(1, std::memory_order_relaxed);
    } else if (sz == 64) {
        gHostSz64.fetch_add(1, std::memory_order_relaxed);
    } else if (sz == 96) {
        gHostSz96.fetch_add(1, std::memory_order_relaxed);
    } else if (sz == 128) {
        gHostSz128.fetch_add(1, std::memory_order_relaxed);
    } else {
        gHostSzOther.fetch_add(1, std::memory_order_relaxed);
    }
}

size_t ObjectAllocSize(BaseObject* obj)
{
    if (obj == nullptr) {
        return 0;
    }
    TypeInfo* tip = PeekTypeInfoAt(reinterpret_cast<uintptr_t>(obj));
    if (!TipLooksValid(tip)) {
        return 0;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    return SaneObjectSize(tip, region);
}

} // namespace

bool B4StaleProbe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_B4STALE");
    return on;
}

void B4StaleProbe::SnapshotBases(const char* point)
{
    if (!Enabled()) {
        return;
    }
    ArmOnce();
    gSnapCalls.fetch_add(1, std::memory_order_relaxed);

    std::unordered_set<uintptr_t> fresh;
    std::unordered_map<uintptr_t, uint32_t> sizes;
    fresh.reserve(1 << 16);
    size_t n = 0;
    Heap::GetHeap().ForEachObj(
        [&](BaseObject* obj) {
            if (obj == nullptr) {
                return;
            }
            uintptr_t p = reinterpret_cast<uintptr_t>(obj);
            if (!Heap::IsHeapAddress(p)) {
                return;
            }
            // Prefer objects that look like real bases (valid tip).
            TypeInfo* tip = PeekTypeInfoAt(p);
            if (!TipLooksValid(tip)) {
                return;
            }
            fresh.insert(p);
            size_t sz = ObjectAllocSize(obj);
            if (sz != 0 && sz <= 0xffffffffu) {
                sizes[p] = static_cast<uint32_t>(sz);
            }
            ++n;
        },
        false);

    {
        std::lock_guard<std::mutex> lk(gMu);
        // Rotate: old latest becomes prior; write into the other buffer.
        if (gUseAAsLatest) {
            // B becomes new latest; A was previous latest → keep as prior via union
            gSnapB.swap(fresh);
            gSnapBPoint = point == nullptr ? "?" : point;
            gSnapBGen = ++gSnapGen;
            gUseAAsLatest = false;
        } else {
            gSnapA.swap(fresh);
            gSnapAPoint = point == nullptr ? "?" : point;
            gSnapAGen = ++gSnapGen;
            gUseAAsLatest = true;
        }
        // Union of all prior bases (for NEVER_VALID judgment).
        const std::unordered_set<uintptr_t>& latest = gUseAAsLatest ? gSnapA : gSnapB;
        for (uintptr_t p : latest) {
            gUnionPrior.insert(p);
        }
        for (const auto& kv : sizes) {
            gPriorBaseSize[kv.first] = kv.second;
        }
        // Cap union growth: if huge, keep only latest two snaps re-merged.
        if (gUnionPrior.size() > (1u << 22)) {
            gUnionPrior.clear();
            for (uintptr_t p : gSnapA) {
                gUnionPrior.insert(p);
            }
            for (uintptr_t p : gSnapB) {
                gUnionPrior.insert(p);
            }
        }
    }

    gSnapBases.fetch_add(n, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(gMu);
        B4S_LOG("SNAP point=%s bases=%zu latestGen=%llu latestSize=%zu priorSize=%zu union=%zu",
                point == nullptr ? "?" : point, n, static_cast<unsigned long long>(gSnapGen),
                (gUseAAsLatest ? gSnapA : gSnapB).size(), (gUseAAsLatest ? gSnapB : gSnapA).size(),
                gUnionPrior.size());
    }
}

void B4StaleProbe::ScanInteriors(const char* point)
{
    if (!Enabled()) {
        return;
    }
    ArmOnce();
    gScanCalls.fetch_add(1, std::memory_order_relaxed);

    // Copy snap views under lock for lock-free classify loop.
    std::unordered_set<uintptr_t> latest;
    std::unordered_set<uintptr_t> prior;
    std::unordered_set<uintptr_t> unionPrior;
    std::unordered_map<uintptr_t, uint32_t> priorSizes;
    {
        std::lock_guard<std::mutex> lk(gMu);
        if (gUseAAsLatest) {
            latest = gSnapA;
            prior = gSnapB;
        } else {
            latest = gSnapB;
            prior = gSnapA;
        }
        unionPrior = gUnionPrior;
        priorSizes = gPriorBaseSize;
    }

    uint64_t scanInterior = 0;
    uint64_t scanStale = 0;
    uint64_t scanNever = 0;
    uint64_t scanSlots = 0;

    Heap::GetHeap().ForEachObj(
        [&](BaseObject* holder) {
            if (holder == nullptr || !holder->IsValidObject() || !holder->HasRefField()) {
                return;
            }
            uintptr_t hBase = reinterpret_cast<uintptr_t>(holder);
            size_t hSize = ObjectAllocSize(holder);
            TypeInfo* hTip = PeekTypeInfoAt(hBase);
            const char* hName = TipName(hTip);

            holder->ForEachRefField([&](RefField<>& field) {
                ++scanSlots;
                gSlots.fetch_add(1, std::memory_order_relaxed);
                BaseObject* raw = field.GetTargetObject();
                uintptr_t val = reinterpret_cast<uintptr_t>(raw);
                if (val == 0 || !Heap::IsHeapAddress(val)) {
                    return;
                }

                Kind k = Kind::Unknown;
                uintptr_t vbase = 0;
                size_t voff = 0;
                TypeInfo* tipB = nullptr;
                Classify(val, k, vbase, voff, tipB);
                if (k != Kind::Interior) {
                    return;
                }

                ++scanInterior;
                gInterior.fetch_add(1, std::memory_order_relaxed);
                CountOffset(voff);

                // Host = object that CONTAINS the interior pointer as its body (tipBase of P).
                // Holder = object with the ref slot. Stale-reuse cares about host N at vbase.
                size_t hostObjSize = 0;
                if (tipB != nullptr) {
                    RegionInfo* r = RegionInfo::TryGetRegionInfoAt(vbase);
                    hostObjSize = SaneObjectSize(tipB, r);
                }
                CountHostSize(hostObjSize);
                (void)hSize;

                bool inLatest = latest.count(val) != 0;
                bool inPrior = prior.count(val) != 0;
                bool inUnion = unionPrior.count(val) != 0;
                bool hostNew = (vbase != 0) && (unionPrior.count(vbase) == 0);
                bool hostOld = (vbase != 0) && (unionPrior.count(vbase) != 0);

                if (inLatest) {
                    gStillBaseNow.fetch_add(1, std::memory_order_relaxed);
                }
                if (inLatest || inPrior) {
                    gInLatestSnap.fetch_add(1, std::memory_order_relaxed);
                }
                if (hostNew) {
                    gHostNotInPrior.fetch_add(1, std::memory_order_relaxed);
                }
                if (hostOld) {
                    gHostInPrior.fetch_add(1, std::memory_order_relaxed);
                }

                // T0 main: P was a legal base in a prior snapshot.
                if (inUnion) {
                    ++scanStale;
                    gStaleConfirmed.fetch_add(1, std::memory_order_relaxed);
                    gInPriorOnly.fetch_add(inPrior && !inLatest ? 1 : 0, std::memory_order_relaxed);
                    bool first = false;
                    {
                        std::lock_guard<std::mutex> lk(gMu);
                        first = gSeenStaleVals.insert(val).second;
                    }
                    if (first || TryTakeDump()) {
                        uint32_t priorSz = 0;
                        auto it = priorSizes.find(val);
                        if (it != priorSizes.end()) {
                            priorSz = it->second;
                        }
                        B4S_LOG("STALE_CONFIRMED point=%s P=%#zx vbase=%#zx voff=%zu hostSz=%zu priorBaseSz=%u "
                                "holder=%#zx hName=%s foff=%zd tipBase=%s inLatest=%d inPrior=%d hostNew=%d",
                                point == nullptr ? "?" : point, static_cast<size_t>(val), static_cast<size_t>(vbase),
                                voff, hostObjSize, priorSz, static_cast<size_t>(hBase), hName,
                                static_cast<ssize_t>(reinterpret_cast<uintptr_t>(&field) - hBase), TipName(tipB),
                                inLatest ? 1 : 0, inPrior ? 1 : 0, hostNew ? 1 : 0);
                    }
                } else {
                    ++scanNever;
                    gNeverValid.fetch_add(1, std::memory_order_relaxed);
                    bool first = false;
                    {
                        std::lock_guard<std::mutex> lk(gMu);
                        first = gSeenNeverVals.insert(val).second;
                    }
                    if (first || TryTakeDump()) {
                        B4S_LOG("NEVER_VALID_BASE point=%s P=%#zx vbase=%#zx voff=%zu hostSz=%zu "
                                "holder=%#zx hName=%s tipBase=%s hostNew=%d",
                                point == nullptr ? "?" : point, static_cast<size_t>(val), static_cast<size_t>(vbase),
                                voff, hostObjSize, static_cast<size_t>(hBase), hName, TipName(tipB), hostNew ? 1 : 0);
                    }
                }

                {
                    std::lock_guard<std::mutex> lk(gMu);
                    gSeenInteriorVals.insert(val);
                }
            });
        },
        false);

    B4S_LOG("SCAN point=%s slots=%llu interior=%llu stale=%llu never=%llu "
            "off8=%llu off16=%llu off24=%llu off32=%llu off40=%llu off48=%llu offOther=%llu "
            "hostSz16=%llu hostSz24=%llu hostSz32=%llu hostSz48=%llu hostSz64=%llu hostSz96=%llu "
            "hostSz128=%llu hostSzOther=%llu hostNew=%llu hostOld=%llu",
            point == nullptr ? "?" : point, static_cast<unsigned long long>(scanSlots),
            static_cast<unsigned long long>(scanInterior), static_cast<unsigned long long>(scanStale),
            static_cast<unsigned long long>(scanNever), static_cast<unsigned long long>(gOff8.load()),
            static_cast<unsigned long long>(gOff16.load()), static_cast<unsigned long long>(gOff24.load()),
            static_cast<unsigned long long>(gOff32.load()), static_cast<unsigned long long>(gOff40.load()),
            static_cast<unsigned long long>(gOff48.load()), static_cast<unsigned long long>(gOffOther.load()),
            static_cast<unsigned long long>(gHostSz16.load()), static_cast<unsigned long long>(gHostSz24.load()),
            static_cast<unsigned long long>(gHostSz32.load()), static_cast<unsigned long long>(gHostSz48.load()),
            static_cast<unsigned long long>(gHostSz64.load()), static_cast<unsigned long long>(gHostSz96.load()),
            static_cast<unsigned long long>(gHostSz128.load()), static_cast<unsigned long long>(gHostSzOther.load()),
            static_cast<unsigned long long>(gHostNotInPrior.load()),
            static_cast<unsigned long long>(gHostInPrior.load()));
}

void B4StaleProbe::FlushSummary(const char* site)
{
    if (!Enabled()) {
        return;
    }
    size_t uniqInterior = 0;
    size_t uniqStale = 0;
    size_t uniqNever = 0;
    {
        std::lock_guard<std::mutex> lk(gMu);
        uniqInterior = gSeenInteriorVals.size();
        uniqStale = gSeenStaleVals.size();
        uniqNever = gSeenNeverVals.size();
    }
    uint64_t stale = gStaleConfirmed.load();
    uint64_t never = gNeverValid.load();
    uint64_t interior = gInterior.load();
    uint64_t off16 = gOff16.load();
    uint64_t offTotal = gOff8.load() + off16 + gOff24.load() + gOff32.load() + gOff40.load() + gOff48.load() +
                        gOffOther.load();
    const char* t0 = "B4S_INCONCLUSIVE";
    if (stale > 0 && never == 0) {
        t0 = "B4S_STALE_CONFIRMED";
    } else if (stale == 0 && never > 0) {
        t0 = "B4S_NEVER_VALID_BASE";
    } else if (stale > 0 && never > 0) {
        t0 = "B4S_MIXED_STALE_AND_NEVER";
    } else if (interior == 0) {
        t0 = "B4S_NO_INTERIOR_SEEN";
    }

    const char* t1 = "B4S_OFFSET_UNKNOWN";
    if (offTotal > 0) {
        // Strict constant-16: off16 == offTotal and host sizes span more than one bucket.
        uint64_t hostBuckets = 0;
        if (gHostSz16.load()) {
            ++hostBuckets;
        }
        if (gHostSz24.load()) {
            ++hostBuckets;
        }
        if (gHostSz32.load()) {
            ++hostBuckets;
        }
        if (gHostSz48.load()) {
            ++hostBuckets;
        }
        if (gHostSz64.load()) {
            ++hostBuckets;
        }
        if (gHostSz96.load()) {
            ++hostBuckets;
        }
        if (gHostSz128.load()) {
            ++hostBuckets;
        }
        if (gHostSzOther.load()) {
            ++hostBuckets;
        }
        if (off16 == offTotal && hostBuckets >= 2) {
            t1 = "B4S_OFFSET_恒定_layout_varies";
        } else if (off16 == offTotal && hostBuckets <= 1) {
            t1 = "B4S_OFFSET_恒定_layout_single";
        } else if (off16 * 100 / offTotal >= 80) {
            t1 = "B4S_OFFSET_布局相关_mostly16";
        } else {
            t1 = "B4S_OFFSET_布局相关_spread";
        }
    }

    B4S_LOG("SUMMARY site=%s snaps=%llu scans=%llu slots=%llu interior=%llu uniqInterior=%zu "
            "stale=%llu uniqStale=%zu never=%llu uniqNever=%zu stillBaseNow=%llu "
            "T0=%s T1=%s off8=%llu off16=%llu off24=%llu off32=%llu offOther=%llu "
            "hostNew=%llu hostOld=%llu",
            site == nullptr ? "?" : site, static_cast<unsigned long long>(gSnapCalls.load()),
            static_cast<unsigned long long>(gScanCalls.load()), static_cast<unsigned long long>(gSlots.load()),
            static_cast<unsigned long long>(interior), uniqInterior, static_cast<unsigned long long>(stale), uniqStale,
            static_cast<unsigned long long>(never), uniqNever,
            static_cast<unsigned long long>(gStillBaseNow.load()), t0, t1,
            static_cast<unsigned long long>(gOff8.load()), static_cast<unsigned long long>(off16),
            static_cast<unsigned long long>(gOff24.load()), static_cast<unsigned long long>(gOff32.load()),
            static_cast<unsigned long long>(gOffOther.load()),
            static_cast<unsigned long long>(gHostNotInPrior.load()),
            static_cast<unsigned long long>(gHostInPrior.load()));
}

} // namespace MapleRuntime
