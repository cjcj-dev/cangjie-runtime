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
    bool holderYoung = false;
    bool valueYoung = false;
    bool valueFree = false;
    bool valueGarbage = false;
    uint8_t phase = 0;
    uint32_t pathTag = 0;
    size_t seq = 0;
    void* returnAddr = nullptr;
};

// Last RecordCrossGenEdge decision + minor consume bits for a slot.
struct RemsetLife {
    uint8_t reason = 255; // 255 = never seen by RecordCrossGenEdge under probe
    uint8_t recorded = 0;
    uint8_t phase = 0;
    uint8_t holderYoung = 0;
    uint8_t valueYoung = 0;
    size_t decisionSeq = 0;
    BaseObject* holder = nullptr;
    BaseObject* value = nullptr;
    uint8_t drain = 0;
    uint8_t live = 0;
    uint8_t rescan = 0;
    uint8_t reffixRemset = 0;
    uint8_t reffixObj = 0;
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
std::atomic<size_t> gRemsetDecisionTotal{ 0 };
std::atomic<size_t> gRemsetRecordedTotal{ 0 };
std::atomic<size_t> gRemsetHolderYoungSkip{ 0 };
std::atomic<size_t> gRemsetDrainHits{ 0 };
std::atomic<size_t> gRemsetLiveHits{ 0 };
std::atomic<size_t> gRemsetRescanHits{ 0 };
std::atomic<size_t> gRemsetReffixHits{ 0 };

std::mutex gMu;
// Inline last-N records per slot/value (no ring-index indirection; avoids wrap stale).
std::unordered_map<MAddress, std::vector<WriteRec>> gBySlot;
std::unordered_map<BaseObject*, std::vector<WriteRec>> gByValue;
std::unordered_map<MAddress, RemsetLife> gRemsetLife;

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
    if (region == nullptr) {
        return false;
    }
    *outYoung = region->IsYoungRegion();
    *outFree = region->IsFreeRegion();
    *outGarbage = region->IsGarbageRegion();
    // Free/garbage/unallocated: tip may be zeroed or reused — do not touch object header.
    if (*outFree || *outGarbage) {
        return false;
    }
    // Align check: object base should sit inside region alloc range.
    MAddress addr = reinterpret_cast<MAddress>(value);
    if (addr < region->GetRegionStart() || addr >= region->GetRegionEnd()) {
        return false;
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
    bool holderYoung = false;
    if (holderHeap) {
        RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
        holderYoung = hr != nullptr && hr->IsYoungRegion();
    }
    const size_t seq = gSeq.fetch_add(1, std::memory_order_relaxed) + 1;

    WriteRec rec;
    rec.slot = slot;
    rec.holder = holder;
    rec.value = value;
    rec.heapValue = heapValue;
    rec.validAtWrite = validAtWrite;
    rec.holderHeap = holderHeap;
    rec.holderValid = holderValid;
    rec.holderYoung = holderYoung;
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
        SW_LOG("LAST_WRITE kind=%s seq=%zu path=%s phase=%u holder=%p holderValid=%u holderYoung=%u "
               "slot=%#zx value=%p validAtWrite=%u heapValue=%u young=%u free=%u garbage=%u ra=%p",
               kind, r.seq, PathName(r.pathTag), static_cast<unsigned>(r.phase), r.holder,
               static_cast<unsigned>(r.holderValid), static_cast<unsigned>(r.holderYoung),
               static_cast<size_t>(r.slot), r.value, static_cast<unsigned>(r.validAtWrite),
               static_cast<unsigned>(r.heapValue), static_cast<unsigned>(r.valueYoung),
               static_cast<unsigned>(r.valueFree), static_cast<unsigned>(r.valueGarbage), r.returnAddr);
    };
    for (const WriteRec& r : slotHits) {
        dump("slot", r);
    }
    for (const WriteRec& r : valueHitsExact) {
        dump("value", r);
    }

    // T0/T1 remset life for this exact slot (register side + consume stages).
    RemsetLife life;
    bool lifeFound = false;
    {
        std::lock_guard<std::mutex> lock(gMu);
        auto lit = gRemsetLife.find(slot);
        if (lit != gRemsetLife.end()) {
            life = lit->second;
            lifeFound = true;
        }
    }
    if (!lifeFound) {
        SW_LOG("REMSET_LIFE slot=%#zx offset=%zd NEVER_SEEN_BY_RECORDCROSSGEN "
               "(no NoteRemsetDecision for this slot under MRT_GCV2_SLOTWRITER)",
               static_cast<size_t>(slot), offset);
        SW_LOG("T0_VERDICT=REGISTER_NEVER_CALLED slot=%#zx", static_cast<size_t>(slot));
    } else {
        static const char* kReasons[] = { "recorded",
                                          "no_young",
                                          "ref_null_or_nonheap",
                                          "ref_not_young",
                                          "holder_null_or_nonheap",
                                          "holder_young",
                                          "unknown",
                                          "no_stamp" };
        const char* rname = (life.reason < 8) ? kReasons[life.reason] : "unset";
        SW_LOG("REMSET_LIFE slot=%#zx offset=%zd reason=%s(%u) recorded=%u phase=%u "
               "holderYoung=%u valueYoung=%u decisionSeq=%zu holder=%p value=%p "
               "drain=%u live=%u rescan=%u reffix_remset=%u reffix_obj=%u",
               static_cast<size_t>(slot), offset, rname, static_cast<unsigned>(life.reason),
               static_cast<unsigned>(life.recorded), static_cast<unsigned>(life.phase),
               static_cast<unsigned>(life.holderYoung), static_cast<unsigned>(life.valueYoung),
               life.decisionSeq, life.holder, life.value, static_cast<unsigned>(life.drain),
               static_cast<unsigned>(life.live), static_cast<unsigned>(life.rescan),
               static_cast<unsigned>(life.reffixRemset), static_cast<unsigned>(life.reffixObj));
        if (life.recorded == 0) {
            SW_LOG("T0_VERDICT=REGISTER_SKIPPED reason=%s holderYoung=%u valueYoung=%u slot=%#zx", rname,
                   static_cast<unsigned>(life.holderYoung), static_cast<unsigned>(life.valueYoung),
                   static_cast<size_t>(slot));
            if (life.reason == 5 /* HOLDER_YOUNG */) {
                SW_LOG("T0_DETAIL=young_to_young_not_in_remset slot=%#zx "
                       "(RecordCrossGenEdge Barrier.cpp:412-416 skips young holder)",
                       static_cast<size_t>(slot));
            }
        } else if (life.drain == 0) {
            SW_LOG("T0_VERDICT=REGISTERED_BUT_NOT_DRAINED slot=%#zx "
                   "(Record ok but slot absent from next DrainForMinor snapshot)",
                   static_cast<size_t>(slot));
        } else if (life.live == 0) {
            SW_LOG("T0_VERDICT=DRAINED_BUT_NOT_LIVE slot=%#zx "
                   "(in rememberedSlots but filtered out of liveRememberedSlots)",
                   static_cast<size_t>(slot));
        } else if (life.reffixRemset == 0 && life.reffixObj == 0) {
            SW_LOG("T1_VERDICT=IN_LIVE_REMSET_BUT_NOT_REFFIXED slot=%#zx "
                   "(live remset had slot; young.ref_fix never visited it as remset or via holder)",
                   static_cast<size_t>(slot));
        } else {
            SW_LOG("T1_VERDICT=REFFIX_VISITED_STILL_STALE slot=%#zx reffix_remset=%u reffix_obj=%u "
                   "(consume side ran FixMinorEvacuatedSlot; value still invalid at enqueue)",
                   static_cast<size_t>(slot), static_cast<unsigned>(life.reffixRemset),
                   static_cast<unsigned>(life.reffixObj));
        }
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

void SlotWriterProbe::NoteRemsetDecision(MAddress slot, BaseObject* holder, BaseObject* value, uint8_t reason,
                                         bool recorded, bool holderYoung, bool valueYoung)
{
    if (!Enabled() || slot == 0) {
        return;
    }
    gRemsetDecisionTotal.fetch_add(1, std::memory_order_relaxed);
    if (recorded) {
        gRemsetRecordedTotal.fetch_add(1, std::memory_order_relaxed);
    }
    if (reason == 5 /* HOLDER_YOUNG */) {
        gRemsetHolderYoungSkip.fetch_add(1, std::memory_order_relaxed);
    }
    const size_t seq = gSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    RemsetLife life;
    life.reason = reason;
    life.recorded = recorded ? 1 : 0;
    life.phase = static_cast<uint8_t>(Heap::GetHeap().GetGCPhase());
    life.holderYoung = holderYoung ? 1 : 0;
    life.valueYoung = valueYoung ? 1 : 0;
    life.decisionSeq = seq;
    life.holder = holder;
    life.value = value;
    {
        std::lock_guard<std::mutex> lock(gMu);
        auto& dest = gRemsetLife[slot];
        // Preserve consume bits across re-decisions on the same slot.
        life.drain = dest.drain;
        life.live = dest.live;
        life.rescan = dest.rescan;
        life.reffixRemset = dest.reffixRemset;
        life.reffixObj = dest.reffixObj;
        dest = life;
    }
}

void SlotWriterProbe::NoteRemsetConsume(MAddress slot, const char* stage)
{
    if (!Enabled() || slot == 0 || stage == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(gMu);
    RemsetLife& life = gRemsetLife[slot];
    if (std::strcmp(stage, "drain") == 0) {
        life.drain = 1;
        gRemsetDrainHits.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(stage, "live") == 0) {
        life.live = 1;
        gRemsetLiveHits.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(stage, "rescan") == 0) {
        life.rescan = 1;
        gRemsetRescanHits.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(stage, "reffix_remset") == 0) {
        life.reffixRemset = 1;
        gRemsetReffixHits.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(stage, "reffix_obj") == 0) {
        life.reffixObj = 1;
        gRemsetReffixHits.fetch_add(1, std::memory_order_relaxed);
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
    // Safety: bulk WriteStruct often runs mid-construct; words may be heap-shaped
    // garbage. NoteRefWrite still runs ValueLooksValid which refuses free/garbage
    // regions before touching tip (see ValueLooksValid).
    MAddress start = (dst + (sizeof(MAddress) - 1)) & ~(static_cast<MAddress>(sizeof(MAddress) - 1));
    MAddress end = dst + dstLen;
    for (MAddress p = start; p + sizeof(MAddress) <= end; p += sizeof(MAddress)) {
        MAddress word = 0;
        std::memcpy(&word, reinterpret_cast<void*>(p), sizeof(word));
        if (word == 0) {
            continue;
        }
        // Drop tagged/non-canonical noise: require plain low-bit clear for ref words.
        if ((word & 0x7ULL) != 0) {
            continue;
        }
        BaseObject* val = reinterpret_cast<BaseObject*>(word);
        if (!Heap::IsHeapAddress(val)) {
            continue;
        }
        // Region must exist and not be free — otherwise tip touch is unsafe.
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(val));
        if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
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
    SW_LOG("REMSET_HITS decisions=%zu recorded=%zu holder_young_skip=%zu "
           "drain=%zu live=%zu rescan=%zu reffix=%zu",
           gRemsetDecisionTotal.load(std::memory_order_relaxed),
           gRemsetRecordedTotal.load(std::memory_order_relaxed),
           gRemsetHolderYoungSkip.load(std::memory_order_relaxed),
           gRemsetDrainHits.load(std::memory_order_relaxed),
           gRemsetLiveHits.load(std::memory_order_relaxed),
           gRemsetRescanHits.load(std::memory_order_relaxed),
           gRemsetReffixHits.load(std::memory_order_relaxed));
}

} // namespace MapleRuntime
