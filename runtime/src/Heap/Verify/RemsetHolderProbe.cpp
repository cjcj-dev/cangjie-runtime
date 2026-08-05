// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RemsetHolderProbe.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "ObjectModel/MClass.h"
#include "ObjectModel/MClass.inline.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {

#define RHOLDER_LOG(fmt, ...)                                                                                          \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][remset-holder] " fmt "\n", ##__VA_ARGS__);                                         \
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
    if (tip->IsRawArray()) {
        return 0; // arrays need length word; fall back to region walk
    }
    MSize isz = tip->GetInstanceSize();
    size_t size = (static_cast<size_t>(isz) + 8u + 7u) & ~static_cast<size_t>(7u);
    size_t regionBytes = region->GetRegionEnd() - region->GetRegionStart();
    if (size < 16 || size > regionBytes || size > (1u << 20)) {
        return 0;
    }
    return size;
}

// Value-side classify (base-first, same rule as InteriorSrcProbe).
enum class ValKind : uint8_t { Base = 0, Interior = 1, Unknown = 2, NotHeap = 3 };

const char* ValKindName(ValKind k)
{
    switch (k) {
        case ValKind::Base:
            return "base";
        case ValKind::Interior:
            return "interior";
        case ValKind::Unknown:
            return "unknown";
        case ValKind::NotHeap:
            return "not_heap";
        default:
            return "?";
    }
}

void ClassifyValue(uintptr_t value, ValKind& kind, uintptr_t& baseOut, size_t& offsetOut, TypeInfo*& tipAtValue,
                   TypeInfo*& tipAtBase)
{
    kind = ValKind::Unknown;
    baseOut = 0;
    offsetOut = 0;
    tipAtValue = nullptr;
    tipAtBase = nullptr;

    if (!Heap::IsHeapAddress(value)) {
        kind = ValKind::NotHeap;
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(value);
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        kind = ValKind::Unknown;
        return;
    }

    tipAtValue = PeekTypeInfoAt(value);
    if (TipLooksValid(tipAtValue)) {
        kind = ValKind::Base;
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
        kind = ValKind::Interior;
        baseOut = cand;
        offsetOut = off;
        tipAtBase = tip;
        return;
    }
    kind = ValKind::Unknown;
}

// Holder verdict for T2 甲/乙/丙.
enum class HolderVerdict : uint8_t {
    ValidFieldHoldsInterior = 0, // 甲
    EntryStaleOrMisaligned = 1,  // 乙
    RecordedNonRefSlot = 2,      // 丙
    HolderNotResolved = 3,
    ClassifierFpAgain = 4, // value is base (not interior) — remset edge is normal
    PosCtrlPass = 5,       // legal edge: H ok + ref-slot + value base
    PosCtrlFail = 6,       // known-legal shape but classifier says bad
};

const char* VerdictName(HolderVerdict v)
{
    switch (v) {
        case HolderVerdict::ValidFieldHoldsInterior:
            return "HOLDER_VALID_FIELD_HOLDS_INTERIOR";
        case HolderVerdict::EntryStaleOrMisaligned:
            return "REMSET_ENTRY_STALE_OR_MISALIGNED";
        case HolderVerdict::RecordedNonRefSlot:
            return "REMSET_RECORDED_NON_REF_SLOT";
        case HolderVerdict::HolderNotResolved:
            return "HOLDER_NOT_RESOLVED";
        case HolderVerdict::ClassifierFpAgain:
            return "CLASSIFIER_FP_AGAIN_value_is_base";
        case HolderVerdict::PosCtrlPass:
            return "POSCTRL_PASS";
        case HolderVerdict::PosCtrlFail:
            return "POSCTRL_FAIL";
        default:
            return "?";
    }
}

std::atomic<uint64_t> gTotal{0};
std::atomic<uint64_t> gDumpLeft{0};
std::atomic<bool> gArmedLogged{false};
std::atomic<uint64_t> gCnt[8]{}; // index by HolderVerdict

// Resolve H by walking the region object stream that contains `slot`.
// Front evidence: VisitAllObjects from region start, never reverse-guess from value.
bool ResolveHolder(uintptr_t slot, BaseObject*& holderOut, size_t& holderSizeOut, TypeInfo*& holderTipOut,
                   bool& tipValidOut, bool& sizeOkOut, const char*& resolveHow)
{
    holderOut = nullptr;
    holderSizeOut = 0;
    holderTipOut = nullptr;
    tipValidOut = false;
    sizeOkOut = false;
    resolveHow = "none";

    if (!Heap::IsHeapAddress(slot)) {
        resolveHow = "slot_not_heap";
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(slot);
    if (region == nullptr) {
        resolveHow = "no_region";
        return false;
    }
    if (region->IsFreeRegion()) {
        resolveHow = "region_free";
        return false;
    }
    if (region->IsGarbageRegion()) {
        resolveHow = "region_garbage";
        return false;
    }

    BaseObject* found = nullptr;
    size_t foundSize = 0;
    // Prefer VisitAllObjects. Guard: only accept objects whose tip looks valid
    // and whose size covers the slot (front evidence from region stream, not reverse-guess).
    region->VisitAllObjects([&](BaseObject* obj) {
        if (found != nullptr || obj == nullptr) {
            return;
        }
        uintptr_t base = reinterpret_cast<uintptr_t>(obj);
        if (slot < base) {
            return;
        }
        TypeInfo* tip = PeekTypeInfoAt(base);
        if (!TipLooksValid(tip)) {
            return;
        }
        // Prefer safe tip-derived size for non-array; fall back to GetAllocSize only when tip ok.
        size_t size = SaneObjectSize(tip, region);
        if (size == 0) {
            // Array / unknown: use GetAllocSize (tip already validated).
            size = RegionSpace::GetAllocSize(*obj);
        }
        if (size < 16 || size > (1u << 20)) {
            return;
        }
        if (slot >= base && slot < base + size) {
            found = obj;
            foundSize = size;
        }
    });

    if (found == nullptr) {
        // Fallback linear walk with tip-only sizes (skip arrays).
        uintptr_t position = region->GetRegionStart();
        uintptr_t allocPtr = region->GetRegionAllocPtr();
        while (position < allocPtr && position + 16 <= allocPtr) {
            TypeInfo* tip = PeekTypeInfoAt(position);
            if (!TipLooksValid(tip)) {
                // Cannot advance safely — stop.
                resolveHow = "stream_bad_tip";
                break;
            }
            size_t size = SaneObjectSize(tip, region);
            if (size == 0) {
                // Array: try GetAllocSize if object looks valid.
                BaseObject* obj = reinterpret_cast<BaseObject*>(position);
                if (obj->IsValidObject()) {
                    size = RegionSpace::GetAllocSize(*obj);
                }
            }
            if (size < 16 || size > (1u << 20) || position + size > region->GetRegionEnd()) {
                resolveHow = "stream_bad_size";
                break;
            }
            if (slot >= position && slot < position + size) {
                found = reinterpret_cast<BaseObject*>(position);
                foundSize = size;
                break;
            }
            position += size;
        }
        if (found == nullptr && std::strcmp(resolveHow, "none") == 0) {
            resolveHow = "slot_not_in_any_object";
        }
    } else {
        resolveHow = "visit_all_objects";
    }

    if (found == nullptr) {
        return false;
    }

    holderOut = found;
    holderSizeOut = foundSize;
    holderTipOut = PeekTypeInfoAt(reinterpret_cast<uintptr_t>(found));
    tipValidOut = TipLooksValid(holderTipOut);
    sizeOkOut = foundSize >= 16 && foundSize <= (1u << 20);
    return true;
}

// Is `slot` one of the ref fields of `holder` according to gctib / ForEachRefField?
bool SlotIsRefField(BaseObject* holder, uintptr_t slot)
{
    if (holder == nullptr) {
        return false;
    }
    if (!holder->IsValidObject()) {
        return false;
    }
    bool hit = false;
    holder->ForEachRefField([&](RefField<>& field) {
        if (reinterpret_cast<uintptr_t>(&field) == slot) {
            hit = true;
        }
    });
    return hit;
}

} // namespace

bool RemsetHolderProbe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_REMSET_HOLDER");
    return on;
}

void RemsetHolderProbe::NoteRemsetEdge(uintptr_t slot, void* value)
{
    if (!Enabled()) {
        return;
    }
    if (!gArmedLogged.exchange(true, std::memory_order_relaxed)) {
        size_t dumpMax = EnvSizeT("MRT_GCV2_REMSET_HOLDER_DUMP_MAX", 64);
        gDumpLeft.store(dumpMax, std::memory_order_relaxed);
        RHOLDER_LOG("ARMED env=MRT_GCV2_REMSET_HOLDER=1 dumpMax=%zu", dumpMax);
    }

    gTotal.fetch_add(1, std::memory_order_relaxed);

    uintptr_t val = reinterpret_cast<uintptr_t>(value);
    ValKind vkind = ValKind::Unknown;
    uintptr_t vbase = 0;
    size_t voff = 0;
    TypeInfo* tipVal = nullptr;
    TypeInfo* tipBase = nullptr;
    ClassifyValue(val, vkind, vbase, voff, tipVal, tipBase);

    BaseObject* holder = nullptr;
    size_t holderSize = 0;
    TypeInfo* holderTip = nullptr;
    bool tipValid = false;
    bool sizeOk = false;
    const char* how = "none";
    bool resolved = ResolveHolder(slot, holder, holderSize, holderTip, tipValid, sizeOk, how);

    RegionInfo* slotRegion = Heap::IsHeapAddress(slot) ? RegionInfo::TryGetRegionInfoAt(slot) : nullptr;
    unsigned regionType = slotRegion != nullptr ? static_cast<unsigned>(slotRegion->GetRegionType()) : 999;
    unsigned young = slotRegion != nullptr ? static_cast<unsigned>(slotRegion->IsYoungRegion()) : 0;
    unsigned free = slotRegion != nullptr ? static_cast<unsigned>(slotRegion->IsFreeRegion()) : 0;
    unsigned garbage = slotRegion != nullptr ? static_cast<unsigned>(slotRegion->IsGarbageRegion()) : 0;
    unsigned ghost = slotRegion != nullptr ? static_cast<unsigned>(slotRegion->IsGhostFromRegion()) : 0;

    bool holderLegal = resolved && tipValid && sizeOk && holder != nullptr && holder->IsValidObject();
    bool inGctib = false;
    size_t fieldOff = 0;
    if (resolved && holder != nullptr) {
        fieldOff = slot - reinterpret_cast<uintptr_t>(holder);
        if (holderLegal) {
            inGctib = SlotIsRefField(holder, slot);
        }
    }

    // Read *slot raw (the remset slot content) without assuming validity.
    uint64_t rawAtSlot = 0;
    if (Heap::IsHeapAddress(slot) && PageMapped(slot)) {
        std::memcpy(&rawAtSlot, reinterpret_cast<const void*>(slot), sizeof(rawAtSlot));
    }

    HolderVerdict verdict = HolderVerdict::HolderNotResolved;
    if (!resolved || !holderLegal) {
        verdict = HolderVerdict::EntryStaleOrMisaligned;
        if (!resolved) {
            // Keep distinct name for "not found at all".
            verdict = HolderVerdict::HolderNotResolved;
        }
    } else if (vkind == ValKind::Base) {
        // Positive control path: legal holder + ref slot + base value.
        if (inGctib) {
            verdict = HolderVerdict::PosCtrlPass;
        } else {
            // Legal H but slot not in gctib, yet value is base — still 丙-ish for the slot,
            // but for value-side classifier this is NOT interior FP.
            verdict = HolderVerdict::RecordedNonRefSlot;
        }
    } else if (vkind == ValKind::Interior) {
        if (inGctib) {
            verdict = HolderVerdict::ValidFieldHoldsInterior; // 甲
        } else {
            verdict = HolderVerdict::RecordedNonRefSlot; // 丙
        }
    } else {
        // value unknown / not_heap with legal holder
        if (inGctib) {
            verdict = HolderVerdict::EntryStaleOrMisaligned; // slot ref-like but value not a sane object
        } else {
            verdict = HolderVerdict::RecordedNonRefSlot;
        }
    }

    // Special: if previous chain said interior but we now see base ⇒ classifier FP again.
    // (Caller may still push; we only diagnose.)
    (void)0;

    gCnt[static_cast<int>(verdict)].fetch_add(1, std::memory_order_relaxed);

    // Dump: always interiors + non-posctrl failures; sample posctrl.
    bool dump = false;
    if (verdict == HolderVerdict::ValidFieldHoldsInterior || verdict == HolderVerdict::EntryStaleOrMisaligned ||
        verdict == HolderVerdict::RecordedNonRefSlot || verdict == HolderVerdict::HolderNotResolved ||
        verdict == HolderVerdict::ClassifierFpAgain || verdict == HolderVerdict::PosCtrlFail) {
        dump = true;
    } else if (verdict == HolderVerdict::PosCtrlPass) {
        uint64_t n = gCnt[static_cast<int>(HolderVerdict::PosCtrlPass)].load(std::memory_order_relaxed);
        dump = (n <= 8) || ((n & 0xff) == 1);
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

    const char* holderName = "?";
    if (holderTip != nullptr && tipValid) {
        const char* n = holderTip->GetName();
        if (n != nullptr) {
            holderName = n;
        }
    }

    GCPhase phase = Heap::GetHeap().GetGCPhase();
    const char* phaseName = Collector::GetGCPhaseName(phase);

    RHOLDER_LOG(
        "EDGE verdict=%s slot=%#zx rawAtSlot=%#llx value=%p vkind=%s vbase=%p voff=%zu "
        "tipVal=%p tipBase=%p holder=%p holderSize=%zu holderTip=%p holderName=%s "
        "holderTipValid=%u holderSizeOk=%u fieldOff=%zu inGctib=%u resolve=%s "
        "regionType=%u young=%u free=%u garbage=%u ghost=%u phase=%s(%u)",
        VerdictName(verdict), static_cast<size_t>(slot), static_cast<unsigned long long>(rawAtSlot), value,
        ValKindName(vkind), reinterpret_cast<void*>(vbase), voff, static_cast<void*>(tipVal),
        static_cast<void*>(tipBase), static_cast<void*>(holder), holderSize, static_cast<void*>(holderTip), holderName,
        static_cast<unsigned>(tipValid), static_cast<unsigned>(sizeOk), fieldOff, static_cast<unsigned>(inGctib), how,
        regionType, young, free, garbage, ghost, phaseName, static_cast<unsigned>(phase));
}

void RemsetHolderProbe::FlushSummary(const char* site)
{
    if (!Enabled()) {
        return;
    }
    RHOLDER_LOG(
        "SUMMARY site=%s total=%llu "
        "甲_VALID_FIELD_INTERIOR=%llu 乙_STALE=%llu 丙_NON_REF=%llu "
        "HOLDER_NOT_RESOLVED=%llu CLASSIFIER_FP_AGAIN=%llu POSCTRL_PASS=%llu POSCTRL_FAIL=%llu",
        site != nullptr ? site : "?", static_cast<unsigned long long>(gTotal.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gCnt[static_cast<int>(HolderVerdict::ValidFieldHoldsInterior)].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gCnt[static_cast<int>(HolderVerdict::EntryStaleOrMisaligned)].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gCnt[static_cast<int>(HolderVerdict::RecordedNonRefSlot)].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gCnt[static_cast<int>(HolderVerdict::HolderNotResolved)].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gCnt[static_cast<int>(HolderVerdict::ClassifierFpAgain)].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gCnt[static_cast<int>(HolderVerdict::PosCtrlPass)].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gCnt[static_cast<int>(HolderVerdict::PosCtrlFail)].load(std::memory_order_relaxed)));
}

} // namespace MapleRuntime
