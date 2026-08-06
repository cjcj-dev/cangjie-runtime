// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "B4CopyProbe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sys/mman.h>
#include <unistd.h>

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

#define B4C_LOG(fmt, ...)                                                                                              \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][b4copy] " fmt "\n", ##__VA_ARGS__);                                                \
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

// base-first classifier (staticinterior / b4persist gold rule).
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
}

std::atomic<bool> gArmed{false};
std::atomic<uint64_t> gDumpLeft{0};
std::atomic<uint64_t> gCopyCalls{0};
std::atomic<uint64_t> gSrcSlots{0};
std::atomic<uint64_t> gSrcInterior{0};
std::atomic<uint64_t> gSrcInteriorOff16{0};
std::atomic<uint64_t> gDstInterior{0};
std::atomic<uint64_t> gDstInteriorOff16{0};
std::atomic<uint64_t> gVerdictPreexisting{0};
std::atomic<uint64_t> gVerdictCopyIntroduces{0};
std::atomic<uint64_t> gVerdictLayoutShift{0};
std::atomic<uint64_t> gVerdictFixupIntroduces{0};
std::atomic<uint64_t> gByteMismatch{0};
std::atomic<uint64_t> gHeaderMismatch{0};
std::atomic<uint64_t> gFixupWrites{0};
std::atomic<uint64_t> gSkipNoTi{0};
std::atomic<uint64_t> gSkipNoRef{0};

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
    size_t dumpMax = EnvSizeT("MRT_GCV2_B4COPY_DUMP_MAX", 64);
    gDumpLeft.store(dumpMax, std::memory_order_relaxed);
    B4C_LOG("ARMED env=MRT_GCV2_B4COPY=1 dumpMax=%zu", dumpMax);
}

const char* TipName(TypeInfo* tip)
{
    if (tip == nullptr || !TipLooksValid(tip)) {
        return "?";
    }
    const char* n = tip->GetName();
    return n != nullptr ? n : "?";
}

// Walk ref fields via GCTib without calling virtual methods that need a live
// TypeInfo state word on a half-built object — use the header word as tip.
bool SafeForEachRefSlot(const BaseObject* obj, size_t objSize,
                        const std::function<void(size_t fieldOff, uintptr_t raw)>& fn)
{
    if (obj == nullptr || objSize < 16) {
        return false;
    }
    uintptr_t base = reinterpret_cast<uintptr_t>(obj);
    TypeInfo* tip = PeekTypeInfoAt(base);
    if (!TipLooksValid(tip)) {
        gSkipNoTi.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!tip->HasRefField()) {
        gSkipNoRef.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Prefer the real ForEachRefField when the object header is coherent.
    const_cast<BaseObject*>(obj)->ForEachRefField([&](RefField<>& field) {
        uintptr_t slot = reinterpret_cast<uintptr_t>(&field);
        if (slot < base || slot + sizeof(void*) > base + objSize) {
            return;
        }
        size_t off = slot - base;
        uintptr_t raw = 0;
        std::memcpy(&raw, reinterpret_cast<const void*>(slot), sizeof(raw));
        fn(off, raw);
    });
    return true;
}

} // namespace

bool B4CopyProbe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_B4COPY");
    return on;
}

void B4CopyProbe::NotePreCopy(const BaseObject& fromObj, size_t size)
{
    if (!Enabled()) {
        return;
    }
    ArmOnce();
    gCopyCalls.fetch_add(1, std::memory_order_relaxed);

    SafeForEachRefSlot(&fromObj, size, [&](size_t fieldOff, uintptr_t raw) {
        (void)fieldOff;
        gSrcSlots.fetch_add(1, std::memory_order_relaxed);
        Kind k = Kind::Unknown;
        uintptr_t vbase = 0;
        size_t voff = 0;
        TypeInfo* tipB = nullptr;
        Classify(raw, k, vbase, voff, tipB);
        if (k == Kind::Interior) {
            gSrcInterior.fetch_add(1, std::memory_order_relaxed);
            if (voff == 16) {
                gSrcInteriorOff16.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
}

void B4CopyProbe::NotePostCopy(const BaseObject& fromObj, BaseObject& toObj, size_t size)
{
    if (!Enabled()) {
        return;
    }
    ArmOnce();

    uintptr_t from = reinterpret_cast<uintptr_t>(&fromObj);
    uintptr_t to = reinterpret_cast<uintptr_t>(&toObj);

    // Header / full-body fidelity (rules out payload-start / wrong-size copy).
    if (size >= 8) {
        uint64_t hFrom = 0;
        uint64_t hTo = 0;
        std::memcpy(&hFrom, reinterpret_cast<const void*>(from), sizeof(hFrom));
        std::memcpy(&hTo, reinterpret_cast<const void*>(to), sizeof(hTo));
        if (hFrom != hTo) {
            gHeaderMismatch.fetch_add(1, std::memory_order_relaxed);
            if (TryTakeDump()) {
                B4C_LOG("HEADER_MISMATCH from=%p to=%p size=%zu hFrom=%#llx hTo=%#llx", &fromObj, &toObj, size,
                        static_cast<unsigned long long>(hFrom), static_cast<unsigned long long>(hTo));
            }
        }
    }
    if (std::memcmp(reinterpret_cast<const void*>(from), reinterpret_cast<const void*>(to), size) != 0) {
        gByteMismatch.fetch_add(1, std::memory_order_relaxed);
        if (TryTakeDump()) {
            B4C_LOG("BYTE_MISMATCH from=%p to=%p size=%zu", &fromObj, &toObj, size);
        }
    }

    // Pairwise field classification: same relative offsets on src vs dst.
    SafeForEachRefSlot(&fromObj, size, [&](size_t fieldOff, uintptr_t srcRaw) {
        if (fieldOff + sizeof(void*) > size) {
            return;
        }
        uintptr_t dstRaw = 0;
        std::memcpy(&dstRaw, reinterpret_cast<const void*>(to + fieldOff), sizeof(dstRaw));

        Kind sk = Kind::Unknown;
        Kind dk = Kind::Unknown;
        uintptr_t sbase = 0;
        uintptr_t dbase = 0;
        size_t soff = 0;
        size_t doff = 0;
        TypeInfo* stip = nullptr;
        TypeInfo* dtip = nullptr;
        Classify(srcRaw, sk, sbase, soff, stip);
        Classify(dstRaw, dk, dbase, doff, dtip);

        if (srcRaw != dstRaw) {
            // memmove must preserve words; disagreement = layout/size bug or concurrent mutator.
            gVerdictLayoutShift.fetch_add(1, std::memory_order_relaxed);
            if (TryTakeDump()) {
                B4C_LOG("LAYOUT_SHIFT holderFrom=%p holderTo=%p foff=%zu src=%#zx dst=%#zx sk=%s dk=%s", &fromObj,
                        &toObj, fieldOff, static_cast<size_t>(srcRaw), static_cast<size_t>(dstRaw), KindName(sk),
                        KindName(dk));
            }
        }

        if (dk == Kind::Interior) {
            gDstInterior.fetch_add(1, std::memory_order_relaxed);
            if (doff == 16) {
                gDstInteriorOff16.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (sk == Kind::Interior && dk == Kind::Interior) {
            gVerdictPreexisting.fetch_add(1, std::memory_order_relaxed);
            if (TryTakeDump()) {
                B4C_LOG("PREEXISTING holderFrom=%p holderTo=%p foff=%zu val=%#zx vbase=%#zx voff=%zu tip=%s", &fromObj,
                        &toObj, fieldOff, static_cast<size_t>(srcRaw), static_cast<size_t>(sbase), soff, TipName(stip));
            }
        } else if (sk == Kind::Base && dk == Kind::Interior) {
            gVerdictCopyIntroduces.fetch_add(1, std::memory_order_relaxed);
            if (TryTakeDump()) {
                B4C_LOG("COPY_INTRODUCES holderFrom=%p holderTo=%p foff=%zu src=%#zx dst=%#zx dbase=%#zx doff=%zu "
                        "stip=%s dtip=%s",
                        &fromObj, &toObj, fieldOff, static_cast<size_t>(srcRaw), static_cast<size_t>(dstRaw),
                        static_cast<size_t>(dbase), doff, TipName(stip), TipName(dtip));
            }
        } else if (sk != Kind::Interior && dk == Kind::Interior) {
            // src unknown/not_heap → dst interior: still count as copy-side introduction signal
            gVerdictCopyIntroduces.fetch_add(1, std::memory_order_relaxed);
            if (TryTakeDump()) {
                B4C_LOG("COPY_INTRODUCES_LOOSE holderFrom=%p holderTo=%p foff=%zu src=%#zx(%s) dst=%#zx dbase=%#zx "
                        "doff=%zu",
                        &fromObj, &toObj, fieldOff, static_cast<size_t>(srcRaw), KindName(sk),
                        static_cast<size_t>(dstRaw), static_cast<size_t>(dbase), doff);
            }
        }
    });
}

void B4CopyProbe::NoteFixupWrite(BaseObject* holder, void* slot, void* newVal, const char* site)
{
    if (!Enabled() || slot == nullptr) {
        return;
    }
    ArmOnce();
    gFixupWrites.fetch_add(1, std::memory_order_relaxed);

    uintptr_t raw = reinterpret_cast<uintptr_t>(newVal);
    Kind k = Kind::Unknown;
    uintptr_t vbase = 0;
    size_t voff = 0;
    TypeInfo* tipB = nullptr;
    Classify(raw, k, vbase, voff, tipB);
    if (k != Kind::Interior) {
        return;
    }
    gVerdictFixupIntroduces.fetch_add(1, std::memory_order_relaxed);
    if (TryTakeDump()) {
        size_t foff = 0;
        if (holder != nullptr) {
            uintptr_t hb = reinterpret_cast<uintptr_t>(holder);
            uintptr_t s = reinterpret_cast<uintptr_t>(slot);
            if (s >= hb) {
                foff = s - hb;
            }
        }
        B4C_LOG("FIXUP_INTRODUCES holder=%p slot=%p foff=%zu val=%#zx vbase=%#zx voff=%zu tip=%s site=%s", holder, slot,
                foff, static_cast<size_t>(raw), static_cast<size_t>(vbase), voff, TipName(tipB),
                site != nullptr ? site : "?");
    }
}

void B4CopyProbe::FlushSummary(const char* site)
{
    if (!Enabled()) {
        return;
    }
    B4C_LOG("SUMMARY site=%s copyCalls=%llu srcSlots=%llu srcInterior=%llu srcOff16=%llu dstInterior=%llu "
            "dstOff16=%llu preexisting=%llu copyIntroduces=%llu layoutShift=%llu fixupIntroduces=%llu "
            "byteMismatch=%llu headerMismatch=%llu fixupWrites=%llu skipNoTi=%llu skipNoRef=%llu",
            site != nullptr ? site : "?", static_cast<unsigned long long>(gCopyCalls.load()),
            static_cast<unsigned long long>(gSrcSlots.load()),
            static_cast<unsigned long long>(gSrcInterior.load()),
            static_cast<unsigned long long>(gSrcInteriorOff16.load()),
            static_cast<unsigned long long>(gDstInterior.load()),
            static_cast<unsigned long long>(gDstInteriorOff16.load()),
            static_cast<unsigned long long>(gVerdictPreexisting.load()),
            static_cast<unsigned long long>(gVerdictCopyIntroduces.load()),
            static_cast<unsigned long long>(gVerdictLayoutShift.load()),
            static_cast<unsigned long long>(gVerdictFixupIntroduces.load()),
            static_cast<unsigned long long>(gByteMismatch.load()),
            static_cast<unsigned long long>(gHeaderMismatch.load()),
            static_cast<unsigned long long>(gFixupWrites.load()),
            static_cast<unsigned long long>(gSkipNoTi.load()),
            static_cast<unsigned long long>(gSkipNoRef.load()));
}

namespace {
void AtexitFlush()
{
    B4CopyProbe::FlushSummary("atexit");
}
struct AtexitRegistrar {
    AtexitRegistrar()
    {
        if (B4CopyProbe::Enabled()) {
            std::atexit(AtexitFlush);
        }
    }
};
AtexitRegistrar gAtexitRegistrar;
} // namespace

} // namespace MapleRuntime
