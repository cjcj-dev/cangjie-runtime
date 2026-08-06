// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "B4PersistProbe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include "Base/Log.h"
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

#define B4P_LOG(fmt, ...)                                                                                              \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][b4persist] " fmt "\n", ##__VA_ARGS__);                                             \
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

// base-first classifier (InteriorSrcProbe / clsaudit gold).
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
}

enum class WriterKind : uint8_t { None = 0, Typed = 1, Bulk = 2 };

struct SlotRec {
    std::atomic<uint64_t> seq{0};
    std::atomic<uintptr_t> value{0};
    std::atomic<uint8_t> writer{static_cast<uint8_t>(WriterKind::None)};
    std::atomic<uint8_t> vkind{static_cast<uint8_t>(Kind::Unknown)};
    std::atomic<uintptr_t> ra0{0};
    std::atomic<uintptr_t> ra1{0};
    std::atomic<uintptr_t> ra2{0};
    std::atomic<const char*> site{nullptr};
};

std::atomic<SlotRec*> gTable{nullptr};
std::atomic<size_t> gCap{0};
std::atomic<uint64_t> gSeq{1};
std::atomic<bool> gArmed{false};
std::atomic<uint64_t> gDumpLeft{0};

std::atomic<uint64_t> gTypedWrites{0};
std::atomic<uint64_t> gBulkNotes{0};
std::atomic<uint64_t> gBulkSlots{0};
std::atomic<uint64_t> gRemsetSeen{0};
std::atomic<uint64_t> gRemsetInterior{0};
std::atomic<uint64_t> gRemsetInteriorOff16{0};
std::atomic<uint64_t> gVerdictWrittenInterior{0};
std::atomic<uint64_t> gVerdictMutatedNoWrite{0};
std::atomic<uint64_t> gVerdictRewritten{0};
std::atomic<uint64_t> gVerdictBulkLast{0};
std::atomic<uint64_t> gVerdictNoRecord{0};
std::atomic<uint64_t> gTargetNode{0};
std::atomic<uint64_t> gTargetAutoEnv{0};
std::atomic<uint64_t> gTargetArray{0};
std::atomic<uint64_t> gTargetRawArray{0};
std::atomic<uint64_t> gTargetOther{0};
std::atomic<uint64_t> gTargetUnknown{0};

void EnsureTable()
{
    SlotRec* expected = nullptr;
    if (gTable.load(std::memory_order_acquire) != nullptr) {
        return;
    }
    size_t cap = EnvSizeT("MRT_GCV2_B4PERSIST_CAP", 1u << 20);
    if (cap < 1024) {
        cap = 1024;
    }
    // power of two
    size_t p2 = 1;
    while (p2 < cap) {
        p2 <<= 1;
    }
    cap = p2;
    SlotRec* t = new (std::nothrow) SlotRec[cap];
    if (t == nullptr) {
        return;
    }
    if (gTable.compare_exchange_strong(expected, t, std::memory_order_acq_rel)) {
        gCap.store(cap, std::memory_order_release);
        size_t dumpMax = EnvSizeT("MRT_GCV2_B4PERSIST_DUMP_MAX", 64);
        gDumpLeft.store(dumpMax, std::memory_order_relaxed);
        if (!gArmed.exchange(true, std::memory_order_relaxed)) {
            B4P_LOG("ARMED env=MRT_GCV2_B4PERSIST=1 cap=%zu dumpMax=%zu", cap, dumpMax);
        }
    } else {
        delete[] t;
    }
}

// Keyed table: store slot key in a parallel atomic array.
std::atomic<uintptr_t>* gKeys = nullptr;

void EnsureKeys(size_t cap)
{
    if (gKeys != nullptr) {
        return;
    }
    auto* keys = new (std::nothrow) std::atomic<uintptr_t>[cap];
    if (keys == nullptr) {
        return;
    }
    for (size_t i = 0; i < cap; ++i) {
        keys[i].store(0, std::memory_order_relaxed);
    }
    // racy but table already published; first writer wins keys array
    static std::atomic<bool> once{false};
    if (!once.exchange(true, std::memory_order_acq_rel)) {
        gKeys = keys;
    } else {
        delete[] keys;
    }
}

SlotRec* FindOrInsert(uintptr_t slot)
{
    EnsureTable();
    SlotRec* t = gTable.load(std::memory_order_acquire);
    size_t cap = gCap.load(std::memory_order_acquire);
    if (t == nullptr || cap == 0) {
        return nullptr;
    }
    EnsureKeys(cap);
    if (gKeys == nullptr) {
        return nullptr;
    }
    size_t mask = cap - 1;
    size_t idx = (slot >> 3) & mask;
    for (size_t probe = 0; probe < 32; ++probe) {
        size_t j = (idx + probe) & mask;
        uintptr_t k = gKeys[j].load(std::memory_order_acquire);
        if (k == slot) {
            return &t[j];
        }
        if (k == 0) {
            uintptr_t expected = 0;
            if (gKeys[j].compare_exchange_strong(expected, slot, std::memory_order_acq_rel)) {
                return &t[j];
            }
            if (expected == slot) {
                return &t[j];
            }
        }
    }
    // fallback overwrite home
    gKeys[idx].store(slot, std::memory_order_release);
    return &t[idx];
}

SlotRec* FindOnly(uintptr_t slot)
{
    SlotRec* t = gTable.load(std::memory_order_acquire);
    size_t cap = gCap.load(std::memory_order_acquire);
    if (t == nullptr || cap == 0 || gKeys == nullptr) {
        return nullptr;
    }
    size_t mask = cap - 1;
    size_t idx = (slot >> 3) & mask;
    for (size_t probe = 0; probe < 32; ++probe) {
        size_t j = (idx + probe) & mask;
        if (gKeys[j].load(std::memory_order_acquire) == slot) {
            return &t[j];
        }
    }
    return nullptr;
}

void CountTargetType(TypeInfo* tipBase)
{
    if (tipBase == nullptr || !TipLooksValid(tipBase)) {
        gTargetUnknown.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const char* name = tipBase->GetName();
    if (name == nullptr) {
        gTargetUnknown.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (std::strstr(name, "Node") != nullptr) {
        gTargetNode.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strstr(name, "AutoEnv") != nullptr || std::strstr(name, "Closure") != nullptr) {
        gTargetAutoEnv.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strstr(name, "Array") != nullptr || tipBase->IsArrayType() || tipBase->IsRawArray()) {
        if (tipBase->IsRawArray()) {
            gTargetRawArray.fetch_add(1, std::memory_order_relaxed);
        } else {
            gTargetArray.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        gTargetOther.fetch_add(1, std::memory_order_relaxed);
    }
}

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

} // namespace

bool B4PersistProbe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_B4PERSIST");
    return on;
}

void B4PersistProbe::NoteTypedWrite(void* slot, void* value, const char* kind, void* ra0, void* ra1, void* ra2)
{
    if (!Enabled() || slot == nullptr) {
        return;
    }
    uintptr_t slotU = reinterpret_cast<uintptr_t>(slot);
    if ((slotU & 7u) != 0) {
        return;
    }
    uintptr_t val = reinterpret_cast<uintptr_t>(value);
    Kind k = Kind::Unknown;
    uintptr_t base = 0;
    size_t off = 0;
    TypeInfo* tipV = nullptr;
    TypeInfo* tipB = nullptr;
    Classify(val, k, base, off, tipV, tipB);

    SlotRec* rec = FindOrInsert(slotU);
    if (rec == nullptr) {
        return;
    }
    uint64_t seq = gSeq.fetch_add(1, std::memory_order_relaxed);
    rec->seq.store(seq, std::memory_order_release);
    rec->value.store(val, std::memory_order_release);
    rec->writer.store(static_cast<uint8_t>(WriterKind::Typed), std::memory_order_release);
    rec->vkind.store(static_cast<uint8_t>(k), std::memory_order_release);
    rec->ra0.store(reinterpret_cast<uintptr_t>(ra0), std::memory_order_relaxed);
    rec->ra1.store(reinterpret_cast<uintptr_t>(ra1), std::memory_order_relaxed);
    rec->ra2.store(reinterpret_cast<uintptr_t>(ra2), std::memory_order_relaxed);
    rec->site.store(kind != nullptr ? kind : "typed", std::memory_order_relaxed);
    gTypedWrites.fetch_add(1, std::memory_order_relaxed);

    // Dump rare interior installs immediately (should be ~0 per interiorwriter).
    if (k == Kind::Interior && TryTakeDump()) {
        const char* tname = "?";
        if (tipB != nullptr && TipLooksValid(tipB)) {
            tname = tipB->GetName();
        }
        B4P_LOG("TYPED_INTERIOR kind=%s slot=%p value=%p vbase=%p voff=%zu tipBaseName=%s ra0=%p ra1=%p ra2=%p",
                kind != nullptr ? kind : "?", slot, value, reinterpret_cast<void*>(base), off, tname, ra0, ra1, ra2);
    }
}

void B4PersistProbe::NoteBulkRange(MAddress start, size_t size, const char* site)
{
    if (!Enabled() || size == 0) {
        return;
    }
    EnsureTable();
    gBulkNotes.fetch_add(1, std::memory_order_relaxed);
    uintptr_t s = static_cast<uintptr_t>(start);
    uintptr_t end = s + size;
    // Align up to 8
    uintptr_t cur = (s + 7u) & ~static_cast<uintptr_t>(7u);
    size_t n = 0;
    while (cur + 8 <= end) {
        SlotRec* rec = FindOrInsert(cur);
        if (rec != nullptr) {
            uint64_t seq = gSeq.fetch_add(1, std::memory_order_relaxed);
            rec->seq.store(seq, std::memory_order_release);
            // bulk does not know per-slot value; mark writer=bulk, clear value.
            rec->value.store(0, std::memory_order_release);
            rec->writer.store(static_cast<uint8_t>(WriterKind::Bulk), std::memory_order_release);
            rec->vkind.store(static_cast<uint8_t>(Kind::Unknown), std::memory_order_release);
            rec->ra0.store(reinterpret_cast<uintptr_t>(__builtin_return_address(0)), std::memory_order_relaxed);
            rec->ra1.store(reinterpret_cast<uintptr_t>(__builtin_return_address(1)), std::memory_order_relaxed);
            rec->ra2.store(reinterpret_cast<uintptr_t>(__builtin_return_address(2)), std::memory_order_relaxed);
            rec->site.store(site != nullptr ? site : "bulk", std::memory_order_relaxed);
            ++n;
        }
        cur += 8;
        if (n > (1u << 16)) {
            break; // safety
        }
    }
    gBulkSlots.fetch_add(n, std::memory_order_relaxed);
}

void B4PersistProbe::NoteRemsetConsume(MAddress slot, void* target)
{
    if (!Enabled()) {
        return;
    }
    gRemsetSeen.fetch_add(1, std::memory_order_relaxed);
    uintptr_t slotU = static_cast<uintptr_t>(slot);
    uintptr_t val = reinterpret_cast<uintptr_t>(target);

    Kind k = Kind::Unknown;
    uintptr_t base = 0;
    size_t off = 0;
    TypeInfo* tipV = nullptr;
    TypeInfo* tipB = nullptr;
    Classify(val, k, base, off, tipV, tipB);

    if (k != Kind::Interior) {
        return;
    }
    gRemsetInterior.fetch_add(1, std::memory_order_relaxed);
    if (off == 16) {
        gRemsetInteriorOff16.fetch_add(1, std::memory_order_relaxed);
    }
    CountTargetType(tipB);

    SlotRec* rec = FindOnly(slotU);
    const char* verdict = "B4P_NO_RECORD";
    WriterKind wk = WriterKind::None;
    Kind writeKind = Kind::Unknown;
    uintptr_t writeVal = 0;
    uint64_t writeSeq = 0;
    uintptr_t ra0 = 0;
    uintptr_t ra1 = 0;
    uintptr_t ra2 = 0;
    const char* site = "none";

    if (rec == nullptr) {
        gVerdictNoRecord.fetch_add(1, std::memory_order_relaxed);
        verdict = "B4P_NO_RECORD";
    } else {
        writeSeq = rec->seq.load(std::memory_order_acquire);
        writeVal = rec->value.load(std::memory_order_acquire);
        wk = static_cast<WriterKind>(rec->writer.load(std::memory_order_acquire));
        writeKind = static_cast<Kind>(rec->vkind.load(std::memory_order_acquire));
        ra0 = rec->ra0.load(std::memory_order_relaxed);
        ra1 = rec->ra1.load(std::memory_order_relaxed);
        ra2 = rec->ra2.load(std::memory_order_relaxed);
        const char* s = rec->site.load(std::memory_order_relaxed);
        if (s != nullptr) {
            site = s;
        }

        if (wk == WriterKind::Typed) {
            if (writeVal == val && writeKind == Kind::Interior) {
                verdict = "B4P_WRITTEN_INTERIOR";
                gVerdictWrittenInterior.fetch_add(1, std::memory_order_relaxed);
            } else if (writeVal == val && writeKind == Kind::Base) {
                // same bits classified base at write, interior now — classifier race/time, rare
                verdict = "B4P_RECLASSIFIED_SAME_BITS";
                gVerdictRewritten.fetch_add(1, std::memory_order_relaxed);
            } else if (writeVal != val) {
                // last typed write left a different value; nobody typed-wrote the interior
                // ⇒ either bulk after (but then writer would be bulk) or silent mutation
                verdict = "B4P_MUTATED_WITHOUT_WRITE";
                gVerdictMutatedNoWrite.fetch_add(1, std::memory_order_relaxed);
            } else {
                verdict = "B4P_TYPED_OTHER";
                gVerdictRewritten.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (wk == WriterKind::Bulk) {
            // last cover was bulk; value at consume is interior
            verdict = "B4P_BULK_LAST";
            gVerdictBulkLast.fetch_add(1, std::memory_order_relaxed);
        } else {
            gVerdictNoRecord.fetch_add(1, std::memory_order_relaxed);
            verdict = "B4P_NO_RECORD";
        }
    }

    const char* tname = "?";
    if (tipB != nullptr && TipLooksValid(tipB)) {
        tname = tipB->GetName() != nullptr ? tipB->GetName() : "?";
    }
    bool isArray = tipB != nullptr && TipLooksValid(tipB) &&
                   (tipB->IsArrayType() || tipB->IsRawArray() ||
                    (tname != nullptr && std::strstr(tname, "Array") != nullptr));

    if (TryTakeDump()) {
        B4P_LOG("REMSET_INTERIOR verdict=%s slot=%#zx value=%p vbase=%p voff=%zu tipBaseName=%s isArray=%d "
                "writeSeq=%llu writeVal=%p writeKind=%s writer=%s site=%s ra0=%p ra1=%p ra2=%p",
                verdict, static_cast<size_t>(slotU), target, reinterpret_cast<void*>(base), off, tname,
                static_cast<int>(isArray), static_cast<unsigned long long>(writeSeq),
                reinterpret_cast<void*>(writeVal), KindName(writeKind),
                wk == WriterKind::Typed ? "typed" : (wk == WriterKind::Bulk ? "bulk" : "none"), site,
                reinterpret_cast<void*>(ra0), reinterpret_cast<void*>(ra1), reinterpret_cast<void*>(ra2));
    }
}

void B4PersistProbe::FlushSummary(const char* site)
{
    if (!Enabled()) {
        return;
    }
    B4P_LOG("SUMMARY site=%s typedWrites=%llu bulkNotes=%llu bulkSlots=%llu remsetSeen=%llu "
            "remsetInterior=%llu remsetOff16=%llu "
            "verdict_WRITTEN_INTERIOR=%llu MUTATED_WITHOUT_WRITE=%llu REWRITTEN=%llu BULK_LAST=%llu NO_RECORD=%llu "
            "target_Node=%llu AutoEnv=%llu Array=%llu RawArray=%llu Other=%llu Unknown=%llu",
            site != nullptr ? site : "?", static_cast<unsigned long long>(gTypedWrites.load()),
            static_cast<unsigned long long>(gBulkNotes.load()), static_cast<unsigned long long>(gBulkSlots.load()),
            static_cast<unsigned long long>(gRemsetSeen.load()),
            static_cast<unsigned long long>(gRemsetInterior.load()),
            static_cast<unsigned long long>(gRemsetInteriorOff16.load()),
            static_cast<unsigned long long>(gVerdictWrittenInterior.load()),
            static_cast<unsigned long long>(gVerdictMutatedNoWrite.load()),
            static_cast<unsigned long long>(gVerdictRewritten.load()),
            static_cast<unsigned long long>(gVerdictBulkLast.load()),
            static_cast<unsigned long long>(gVerdictNoRecord.load()),
            static_cast<unsigned long long>(gTargetNode.load()),
            static_cast<unsigned long long>(gTargetAutoEnv.load()),
            static_cast<unsigned long long>(gTargetArray.load()),
            static_cast<unsigned long long>(gTargetRawArray.load()),
            static_cast<unsigned long long>(gTargetOther.load()),
            static_cast<unsigned long long>(gTargetUnknown.load()));
}

} // namespace MapleRuntime
