// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "SlotWriterProbe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "ObjectModel/MClass.h"

namespace MapleRuntime {
namespace {

#define SW_LOG(fmt, ...)                                                                                               \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][slotwriter] " fmt "\n", ##__VA_ARGS__);                                            \
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

struct WriteRec {
    MAddress slot = 0;
    BaseObject* holder = nullptr;
    BaseObject* value = nullptr;
    bool heapValue = false;
    bool validAtWrite = false;
    bool holderHeap = false;
    bool holderValid = false;
    bool valueYoung = false;
    bool valueFree = false;
    bool valueGarbage = false;
    uint8_t phase = 0;
    uint32_t pathTag = 0;
    size_t seq = 0;
    void* returnAddr = nullptr;
};

constexpr size_t kPerSlotCap = 8;

std::atomic<int> gEnabled{ -1 };
std::atomic<size_t> gSeq{ 0 };
std::atomic<size_t> gWriteTotal{ 0 };
std::atomic<size_t> gWriteValid{ 0 };
std::atomic<size_t> gWriteInvalid{ 0 };
std::atomic<size_t> gWriteNull{ 0 };
std::atomic<size_t> gInvalidEnqueueHits{ 0 };
std::atomic<size_t> gInvalidEnqueueMiss{ 0 };
std::atomic<size_t> gDumpsLeft{ 0 };

std::mutex gMu;
// Inline last-N records per slot/value (no ring-index indirection; avoids wrap stale).
std::unordered_map<MAddress, std::vector<WriteRec>> gBySlot;
std::unordered_map<BaseObject*, std::vector<WriteRec>> gByValue;

// Path tags 1..16 — keep stable; SUMMARY dumps hit counts for positive control.
enum PathTagId : uint32_t {
    PT_WriteReference = 1,
    PT_AtomicWrite = 2,
    PT_AtomicSwap = 3,
    PT_CAS = 4,
    PT_WriteStruct = 5,
    PT_gc_cas = 6, // legacy alias
    PT_WriteStatic = 7,
    PT_CasInstallPlain = 8,
    PT_FixMinorSlot = 9,
    PT_TryUpdateRef = 10,
    PT_TraceTag = 11,
    PT_FixOldTag = 12,
    PT_ForwardRoot = 13,
    PT_EnumTag = 14,
    PT_UntagRef = 15,
    PT_WriteStructWord = 16,
    PT_other = 99,
};

constexpr size_t kPathHitSlots = 17; // index by tag 1..16
std::atomic<size_t> gPathHits[kPathHitSlots];

uint32_t PathTag(const char* path)
{
    if (path == nullptr) {
        return 0;
    }
    if (std::strcmp(path, "WriteReference") == 0) {
        return PT_WriteReference;
    }
    if (std::strcmp(path, "AtomicWrite") == 0) {
        return PT_AtomicWrite;
    }
    if (std::strcmp(path, "AtomicSwap") == 0) {
        return PT_AtomicSwap;
    }
    if (std::strcmp(path, "CAS") == 0) {
        return PT_CAS;
    }
    if (std::strcmp(path, "WriteStruct") == 0) {
        return PT_WriteStruct;
    }
    if (std::strcmp(path, "gc_cas") == 0) {
        return PT_gc_cas;
    }
    if (std::strcmp(path, "WriteStatic") == 0) {
        return PT_WriteStatic;
    }
    if (std::strcmp(path, "CasInstallPlain") == 0) {
        return PT_CasInstallPlain;
    }
    if (std::strcmp(path, "FixMinorSlot") == 0) {
        return PT_FixMinorSlot;
    }
    if (std::strcmp(path, "TryUpdateRef") == 0) {
        return PT_TryUpdateRef;
    }
    if (std::strcmp(path, "TraceTag") == 0) {
        return PT_TraceTag;
    }
    if (std::strcmp(path, "FixOldTag") == 0) {
        return PT_FixOldTag;
    }
    if (std::strcmp(path, "ForwardRoot") == 0) {
        return PT_ForwardRoot;
    }
    if (std::strcmp(path, "EnumTag") == 0) {
        return PT_EnumTag;
    }
    if (std::strcmp(path, "UntagRef") == 0) {
        return PT_UntagRef;
    }
    if (std::strcmp(path, "WriteStructWord") == 0) {
        return PT_WriteStructWord;
    }
    return PT_other;
}

const char* PathName(uint32_t tag)
{
    switch (tag) {
        case PT_WriteReference:
            return "WriteReference";
        case PT_AtomicWrite:
            return "AtomicWrite";
        case PT_AtomicSwap:
            return "AtomicSwap";
        case PT_CAS:
            return "CAS";
        case PT_WriteStruct:
            return "WriteStruct";
        case PT_gc_cas:
            return "gc_cas";
        case PT_WriteStatic:
            return "WriteStatic";
        case PT_CasInstallPlain:
            return "CasInstallPlain";
        case PT_FixMinorSlot:
            return "FixMinorSlot";
        case PT_TryUpdateRef:
            return "TryUpdateRef";
        case PT_TraceTag:
            return "TraceTag";
        case PT_FixOldTag:
            return "FixOldTag";
        case PT_ForwardRoot:
            return "ForwardRoot";
        case PT_EnumTag:
            return "EnumTag";
        case PT_UntagRef:
            return "UntagRef";
        case PT_WriteStructWord:
            return "WriteStructWord";
        default:
            return "other";
    }
}

void PushRec(std::vector<WriteRec>& vec, const WriteRec& rec)
{
    if (vec.size() >= kPerSlotCap) {
        vec.erase(vec.begin());
    }
    vec.push_back(rec);
}

bool ValueLooksValid(BaseObject* value, bool* outYoung, bool* outFree, bool* outGarbage)
{
    *outYoung = false;
    *outFree = false;
    *outGarbage = false;
    if (value == nullptr) {
        return false;
    }
    if (!Heap::IsHeapAddress(value)) {
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(value));
    if (region != nullptr) {
        *outYoung = region->IsYoungRegion();
        *outFree = region->IsFreeRegion();
        *outGarbage = region->IsGarbageRegion();
    }
    // Same predicate minor uses: stateWord tip non-null.
    return value->IsValidObject();
}

} // namespace

bool SlotWriterProbe::Enabled()
{
    int v = gEnabled.load(std::memory_order_acquire);
    if (v >= 0) {
        return v != 0;
    }
    bool on = EnvIsOne("MRT_GCV2_SLOTWRITER");
    gEnabled.store(on ? 1 : 0, std::memory_order_release);
    if (on) {
        gDumpsLeft.store(EnvSizeT("MRT_GCV2_SLOTWRITER_DUMP_MAX", 32), std::memory_order_relaxed);
        SW_LOG("ENABLED dump_max=%zu per_slot=%zu", EnvSizeT("MRT_GCV2_SLOTWRITER_DUMP_MAX", 32), kPerSlotCap);
    }
    return on;
}

void SlotWriterProbe::NoteRefWrite(BaseObject* holder, MAddress slot, BaseObject* value, const char* path)
{
    if (!Enabled()) {
        return;
    }
    if (slot == 0) {
        return;
    }

    gWriteTotal.fetch_add(1, std::memory_order_relaxed);
    {
        uint32_t tag = PathTag(path);
        if (tag > 0 && tag < kPathHitSlots) {
            gPathHits[tag].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (value == nullptr) {
        gWriteNull.fetch_add(1, std::memory_order_relaxed);
    }

    bool valueYoung = false;
    bool valueFree = false;
    bool valueGarbage = false;
    const bool heapValue = value != nullptr && Heap::IsHeapAddress(value);
    const bool validAtWrite = heapValue && ValueLooksValid(value, &valueYoung, &valueFree, &valueGarbage);
    if (value != nullptr) {
        if (validAtWrite) {
            gWriteValid.fetch_add(1, std::memory_order_relaxed);
        } else {
            gWriteInvalid.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const bool holderHeap = holder != nullptr && Heap::IsHeapAddress(holder);
    const bool holderValid = holderHeap && holder->IsValidObject();
    const size_t seq = gSeq.fetch_add(1, std::memory_order_relaxed) + 1;

    WriteRec rec;
    rec.slot = slot;
    rec.holder = holder;
    rec.value = value;
    rec.heapValue = heapValue;
    rec.validAtWrite = validAtWrite;
    rec.holderHeap = holderHeap;
    rec.holderValid = holderValid;
    rec.valueYoung = valueYoung;
    rec.valueFree = valueFree;
    rec.valueGarbage = valueGarbage;
    rec.phase = static_cast<uint8_t>(Heap::GetHeap().GetGCPhase());
    rec.pathTag = PathTag(path);
    rec.seq = seq;
    rec.returnAddr = __builtin_return_address(0);

    {
        std::lock_guard<std::mutex> lock(gMu);
        PushRec(gBySlot[slot], rec);
        if (value != nullptr) {
            PushRec(gByValue[value], rec);
        }
    }

    // Log only heap addresses that fail IsValidObject at write time.
    // Non-heap values (code pointers, native) are common and not the T1 bit.
    if (heapValue && !validAtWrite) {
        size_t left = gDumpsLeft.load(std::memory_order_relaxed);
        if (left > 0 && gDumpsLeft.fetch_sub(1, std::memory_order_relaxed) > 0) {
            SW_LOG("WRITE_INVALID seq=%zu path=%s phase=%u holder=%p holderValid=%u slot=%#zx value=%p "
                   "heapValue=%u young=%u free=%u garbage=%u ra=%p",
                   seq, PathName(rec.pathTag), static_cast<unsigned>(rec.phase), holder,
                   static_cast<unsigned>(holderValid), static_cast<size_t>(slot), value,
                   static_cast<unsigned>(heapValue), static_cast<unsigned>(valueYoung),
                   static_cast<unsigned>(valueFree), static_cast<unsigned>(valueGarbage), rec.returnAddr);
        }
    }
}

void SlotWriterProbe::OnInvalidEnqueue(BaseObject* object, BaseObject* holder, MAddress slot, MAddress raw,
                                       const char* origin)
{
    if (!Enabled()) {
        return;
    }

    std::vector<WriteRec> slotHits;
    std::vector<WriteRec> valueHitsExact;
    {
        std::lock_guard<std::mutex> lock(gMu);
        auto sit = gBySlot.find(slot);
        if (sit != gBySlot.end()) {
            slotHits = sit->second;
        }
        auto vit = gByValue.find(object);
        if (vit != gByValue.end()) {
            for (const WriteRec& r : vit->second) {
                // Exact value match only (map key is the pointer identity at write time).
                if (r.value == object) {
                    valueHitsExact.push_back(r);
                }
            }
        }
    }

    const ssize_t offset =
        (holder != nullptr && slot != 0)
            ? static_cast<ssize_t>(slot - reinterpret_cast<MAddress>(holder))
            : static_cast<ssize_t>(-1);

    if (slotHits.empty() && valueHitsExact.empty()) {
        gInvalidEnqueueMiss.fetch_add(1, std::memory_order_relaxed);
        SW_LOG("ENQUEUE_INVALID_NO_WRITE object=%p holder=%p slot=%#zx offset=%zd raw=%#zx origin=%s "
               "writes_total=%zu valid=%zu invalid=%zu null=%zu "
               "HINT=no_barrier_write_seen_for_this_slot_or_exact_value",
               object, holder, static_cast<size_t>(slot), offset, static_cast<size_t>(raw),
               origin == nullptr ? "null" : origin, gWriteTotal.load(std::memory_order_relaxed),
               gWriteValid.load(std::memory_order_relaxed), gWriteInvalid.load(std::memory_order_relaxed),
               gWriteNull.load(std::memory_order_relaxed));
        // Critical T1 signal: slot never seen by barrier write path.
        SW_LOG("T1_VERDICT=NO_BARRIER_WRITE_FOR_SLOT object=%p holder=%p slot=%#zx offset=%zd origin=%s",
               object, holder, static_cast<size_t>(slot), offset, origin == nullptr ? "null" : origin);
        return;
    }

    gInvalidEnqueueHits.fetch_add(1, std::memory_order_relaxed);
    SW_LOG("ENQUEUE_INVALID_MATCH object=%p holder=%p slot=%#zx offset=%zd raw=%#zx origin=%s "
           "slotHits=%zu valueHitsExact=%zu",
           object, holder, static_cast<size_t>(slot), offset, static_cast<size_t>(raw),
           origin == nullptr ? "null" : origin, slotHits.size(), valueHitsExact.size());

    auto dump = [](const char* kind, const WriteRec& r) {
        SW_LOG("LAST_WRITE kind=%s seq=%zu path=%s phase=%u holder=%p holderValid=%u slot=%#zx value=%p "
               "validAtWrite=%u heapValue=%u young=%u free=%u garbage=%u ra=%p",
               kind, r.seq, PathName(r.pathTag), static_cast<unsigned>(r.phase), r.holder,
               static_cast<unsigned>(r.holderValid), static_cast<size_t>(r.slot), r.value,
               static_cast<unsigned>(r.validAtWrite), static_cast<unsigned>(r.heapValue),
               static_cast<unsigned>(r.valueYoung), static_cast<unsigned>(r.valueFree),
               static_cast<unsigned>(r.valueGarbage), r.returnAddr);
    };
    for (const WriteRec& r : slotHits) {
        dump("slot", r);
    }
    for (const WriteRec& r : valueHitsExact) {
        dump("value", r);
    }

    // Prefer slot history for T1: was THIS slot's last write(s) valid?
    bool anyValid = false;
    bool anyInvalid = false;
    bool anyExactValueOnSlot = false;
    const std::vector<WriteRec>& primary = !slotHits.empty() ? slotHits : valueHitsExact;
    for (const WriteRec& r : primary) {
        if (!slotHits.empty() && r.value == object) {
            anyExactValueOnSlot = true;
        }
        if (r.value == nullptr) {
            continue;
        }
        if (r.validAtWrite) {
            anyValid = true;
        } else {
            anyInvalid = true;
        }
    }
    if (!slotHits.empty() && !anyExactValueOnSlot) {
        // Slot was written, but never with this exact value — value arrived another way
        // (struct bulk / plain store / GC cas install) or was last-N evicted.
        SW_LOG("T1_VERDICT=SLOT_WRITTEN_BUT_NOT_THIS_VALUE object=%p slot=%#zx offset=%zd "
               "slotHits=%zu (last slot writes did not store this object pointer)",
               object, static_cast<size_t>(slot), offset, slotHits.size());
    } else if (anyValid && !anyInvalid) {
        SW_LOG("T1_VERDICT=VALID_AT_WRITE_THEN_STALE object=%p slot=%#zx offset=%zd", object,
               static_cast<size_t>(slot), offset);
    } else if (!anyValid && anyInvalid) {
        SW_LOG("T1_VERDICT=INVALID_AT_WRITE object=%p slot=%#zx offset=%zd", object,
               static_cast<size_t>(slot), offset);
    } else if (anyValid && anyInvalid) {
        SW_LOG("T1_VERDICT=MIXED_WRITES object=%p slot=%#zx offset=%zd", object, static_cast<size_t>(slot),
               offset);
    } else {
        SW_LOG("T1_VERDICT=NO_CLASSIFY object=%p slot=%#zx offset=%zd", object, static_cast<size_t>(slot),
               offset);
    }
}

void SlotWriterProbe::NoteStructWords(BaseObject* holder, MAddress dst, size_t dstLen)
{
    if (!Enabled()) {
        return;
    }
    if (dst == 0 || dstLen < sizeof(MAddress)) {
        return;
    }
    // Align up to pointer boundary; scan full words only.
    MAddress start = (dst + (sizeof(MAddress) - 1)) & ~(static_cast<MAddress>(sizeof(MAddress) - 1));
    MAddress end = dst + dstLen;
    for (MAddress p = start; p + sizeof(MAddress) <= end; p += sizeof(MAddress)) {
        MAddress word = 0;
        std::memcpy(&word, reinterpret_cast<void*>(p), sizeof(word));
        if (word == 0) {
            continue;
        }
        BaseObject* val = reinterpret_cast<BaseObject*>(word);
        // Only record heap-looking words (drops immediates / code ptrs noise).
        if (!Heap::IsHeapAddress(val)) {
            continue;
        }
        NoteRefWrite(holder, p, val, "WriteStructWord");
    }
}

void SlotWriterProbe::FlushSummary(const char* site)
{
    if (!Enabled()) {
        return;
    }
    SW_LOG("SUMMARY site=%s writes_total=%zu valid=%zu invalid=%zu null=%zu "
           "enq_hit=%zu enq_miss=%zu",
           site == nullptr ? "?" : site, gWriteTotal.load(std::memory_order_relaxed),
           gWriteValid.load(std::memory_order_relaxed), gWriteInvalid.load(std::memory_order_relaxed),
           gWriteNull.load(std::memory_order_relaxed), gInvalidEnqueueHits.load(std::memory_order_relaxed),
           gInvalidEnqueueMiss.load(std::memory_order_relaxed));
    // Positive-control path hits (T0 self-proof): new hooks must be non-zero in a live run.
    SW_LOG("PATH_HITS WriteReference=%zu AtomicWrite=%zu AtomicSwap=%zu CAS=%zu "
           "WriteStruct=%zu WriteStatic=%zu CasInstallPlain=%zu FixMinorSlot=%zu "
           "TryUpdateRef=%zu TraceTag=%zu FixOldTag=%zu ForwardRoot=%zu EnumTag=%zu "
           "UntagRef=%zu WriteStructWord=%zu",
           gPathHits[PT_WriteReference].load(std::memory_order_relaxed),
           gPathHits[PT_AtomicWrite].load(std::memory_order_relaxed),
           gPathHits[PT_AtomicSwap].load(std::memory_order_relaxed),
           gPathHits[PT_CAS].load(std::memory_order_relaxed),
           gPathHits[PT_WriteStruct].load(std::memory_order_relaxed),
           gPathHits[PT_WriteStatic].load(std::memory_order_relaxed),
           gPathHits[PT_CasInstallPlain].load(std::memory_order_relaxed),
           gPathHits[PT_FixMinorSlot].load(std::memory_order_relaxed),
           gPathHits[PT_TryUpdateRef].load(std::memory_order_relaxed),
           gPathHits[PT_TraceTag].load(std::memory_order_relaxed),
           gPathHits[PT_FixOldTag].load(std::memory_order_relaxed),
           gPathHits[PT_ForwardRoot].load(std::memory_order_relaxed),
           gPathHits[PT_EnumTag].load(std::memory_order_relaxed),
           gPathHits[PT_UntagRef].load(std::memory_order_relaxed),
           gPathHits[PT_WriteStructWord].load(std::memory_order_relaxed));
}

} // namespace MapleRuntime
