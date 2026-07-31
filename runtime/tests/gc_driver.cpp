// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// Synthetic event-sequence driver for the memory manager.
// Events drive allocation / store / drop / force collection / pin.
// Deterministic --seed / --replay / --reduce. No compiler involved.
// Round 2: barrier-only hit search (STORE_PLAIN excluded from hit criterion).

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Allocator/RegionManager.h"
#include "Allocator/RegionSpace.h"
#include "CangjieRuntime.h"
#include "Common/BaseObject.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/StickyLog.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/Flags.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/RefField.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
namespace {

// ---- synthetic TypeInfo: one ref field at offset 0 of payload ----
// Object layout: [TypeInfo* header][RefField slot0]
// instanceSize = sizeof(void*)  (payload only; GetSize adds TYPEINFO_PTR_SIZE)
// GCTib short form: SIGN_BIT | (1<<0)  => bit0 marks first 8-byte payload word as ref

alignas(16) static uint8_t g_typeInfoStorage[512];
static TypeInfo* g_holderTi = nullptr;
static const char* kTypeName = "gcdriver.Holder";

void InitSyntheticTypeInfo()
{
    std::memset(g_typeInfoStorage, 0, sizeof(g_typeInfoStorage));
    g_holderTi = reinterpret_cast<TypeInfo*>(g_typeInfoStorage);
    g_holderTi->SetName(kTypeName);
    g_holderTi->SetType(static_cast<I8>(TypeKind::TYPE_KIND_CLASS));
    g_holderTi->SetFlag(static_cast<I8>(FLAG_HAS_REF_FIELD));
    g_holderTi->SetFieldNum(1);
    g_holderTi->SetInstanceSize(static_cast<U32>(sizeof(void*)));
    g_holderTi->SetAlign(static_cast<U8>(sizeof(void*)));
    g_holderTi->SetUUID(1u);
    GCTib tib {};
    // short bitmap: high bit set + bit0 => first payload word is a ref
    tib.tag = SIGN_BIT | static_cast<ArchUInt>(1);
    g_holderTi->SetGCTib(tib);
}

// ---- event ISA ----
enum class Op : uint8_t {
    ALLOC = 1,       // a,b,c: size_class, kind, unused
    STORE = 2,       // a,b,c: holderIdx, slotIdx, targetIdx
    DROP = 3,        // a: objIdx
    FORCE_MINOR = 4,
    FORCE_MAJOR = 5,
    GROW = 6,        // a: bytes (approx via many small allocs)
    PIN = 7,         // a: objIdx
    UNPIN = 8,       // a: objIdx
    OBSERVE = 9,     // run remset completeness check; fail => "reproduced"
    PROMOTE = 10,    // promote all young regions to old (synthetic age-out)
    STORE_PLAIN = 11,// store without write-barrier / without sticky log
    STORE_BARRIER = 12, // store via Heap barrier (logs when IdleLogBarrier active)
    FILL_REGION = 13, // a: objIdx — pack holder's region to RECENT_FULL via allocs in same region
};

struct Event {
    Op op;
    uint32_t a;
    uint32_t b;
    uint32_t c;
};

enum class AllocKind : uint32_t {
    MOVEABLE = 0,
    PINNED = 1,
    LARGE = 2,
};

// ---- PRNG (xorshift64*, deterministic) ----
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ULL) {}
    uint64_t next()
    {
        uint64_t x = s;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        s = x;
        return x * 0x2545F4914F6CDD1DULL;
    }
    uint32_t next_u32(uint32_t lo, uint32_t hi)
    {
        if (hi <= lo) {
            return lo;
        }
        return lo + static_cast<uint32_t>(next() % (hi - lo + 1));
    }
};

// ---- object table ----
struct Slot {
    BaseObject* obj = nullptr;
    RegionInfo* home = nullptr;
    bool live = false;
    bool pinned = false;
    uint32_t size = 0;
    // provenance of the last write into this holder's ref slot
    bool lastStoreWasBarrier = false;
    bool lastStoreWasPlain = false;
};

struct MissInfo {
    size_t holderIdx = 0;
    BaseObject* holder = nullptr;
    BaseObject* target = nullptr;
    unsigned holderRegionType = 0;
    unsigned targetRegionType = 0;
    uint64_t regionPendingLines = 0;
    bool storeWasBarrier = false;
    bool storeWasPlain = false;
    bool lineLogged = false;
};

struct Driver {
    std::vector<Slot> slots;
    RegionManager* manager = nullptr;
    uint64_t allocCount = 0;
    uint64_t storeCount = 0;
    uint64_t barrierStoreCount = 0;
    uint64_t plainStoreCount = 0;
    uint64_t observeCount = 0;
    uint64_t missingEdges = 0;
    uint64_t loggedEdges = 0;
    uint64_t oldToYoungEdges = 0;
    uint64_t barrierMissing = 0; // missing edges whose last store was STORE_BARRIER
    uint64_t plainMissing = 0;
    bool lastObserveHit = false;          // any missing (compat)
    bool lastBarrierOnlyHit = false;      // barrier-origin + line=0 + region pending=0
    std::string lastMissDetail;
    MissInfo lastMiss {};
    bool requireBarrierOnlyHit = false;   // OBSERVE hit predicate for reduce/search
    bool trackStepTrace = false;
    uint64_t stepIndex = 0;

    void ResetStats()
    {
        allocCount = storeCount = barrierStoreCount = plainStoreCount = observeCount = 0;
        missingEdges = loggedEdges = oldToYoungEdges = barrierMissing = plainMissing = 0;
        lastObserveHit = false;
        lastBarrierOnlyHit = false;
        lastMissDetail.clear();
        lastMiss = MissInfo {};
        stepIndex = 0;
    }

    // Count sticky-log bytes that are non-zero inside [regionStart, regionStart+regionSize).
    static uint64_t CountPendingLinesInRegion(RegionInfo* reg)
    {
        if (reg == nullptr || !reg->IsValidRegion()) {
            return 0;
        }
        StickyLog& log = StickyLog::Instance();
        MAddress start = reg->GetRegionStart();
        size_t size = reg->GetRegionSize();
        uint64_t n = 0;
        for (MAddress a = start; a < start + size; a += StickyLog::LINE_SIZE) {
            if (log.IsLoggedLine(a)) {
                ++n;
            }
        }
        return n;
    }

    int AllocSlot(uint32_t sizeClass, uint32_t kind)
    {
        static const uint32_t kPayload[] = { 8, 8, 16, 32 };
        uint32_t payload = kPayload[sizeClass % 4];
        g_holderTi->SetInstanceSize(payload);
        size_t objSize = payload + TYPEINFO_PTR_SIZE;
        if (objSize < 16) {
            objSize = 16;
        }
        objSize = (objSize + 7u) & ~size_t(7u);

        RegionInfo* region = nullptr;
        if (kind == static_cast<uint32_t>(AllocKind::LARGE)) {
            size_t units = (objSize + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;
            if (units < 1) {
                units = 1;
            }
            region = manager->TakeRegion(units, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
        } else if (kind == static_cast<uint32_t>(AllocKind::PINNED)) {
            region = manager->TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        } else {
            region = manager->TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        }
        if (region == nullptr) {
            std::printf("GCDRIVER alloc FAIL take-region kind=%u\n", kind);
            return -1;
        }
        MAddress addr = region->Alloc(objSize);
        if (addr == 0) {
            region = manager->TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
            if (region == nullptr) {
                return -1;
            }
            addr = region->Alloc(objSize);
            if (addr == 0) {
                return -1;
            }
        }
        BaseObject* obj = reinterpret_cast<BaseObject*>(addr);
        obj->SetClassInfo(g_holderTi);
        auto* slot0 = reinterpret_cast<RefField<>*>(reinterpret_cast<uintptr_t>(obj) + TYPEINFO_PTR_SIZE);
        slot0->SetTargetObject(nullptr);

        if (kind == static_cast<uint32_t>(AllocKind::PINNED)) {
            region->IncRawPointerObjectCount();
        }

        Slot s;
        s.obj = obj;
        s.home = region;
        s.live = true;
        s.pinned = (kind == static_cast<uint32_t>(AllocKind::PINNED));
        s.size = static_cast<uint32_t>(objSize);
        slots.push_back(s);
        ++allocCount;
        return static_cast<int>(slots.size() - 1);
    }

    bool ValidIdx(uint32_t i) const { return i < slots.size() && slots[i].live && slots[i].obj != nullptr; }

    RefField<>* SlotField(BaseObject* obj, uint32_t slotIdx)
    {
        (void)slotIdx;
        return reinterpret_cast<RefField<>*>(reinterpret_cast<uintptr_t>(obj) + TYPEINFO_PTR_SIZE);
    }

    void StorePlain(uint32_t holderIdx, uint32_t slotIdx, uint32_t targetIdx)
    {
        if (!ValidIdx(holderIdx) || !ValidIdx(targetIdx)) {
            return;
        }
        RefField<>* f = SlotField(slots[holderIdx].obj, slotIdx);
        f->SetTargetObject(slots[targetIdx].obj);
        slots[holderIdx].lastStoreWasBarrier = false;
        slots[holderIdx].lastStoreWasPlain = true;
        ++storeCount;
        ++plainStoreCount;
    }

    void StoreBarrier(uint32_t holderIdx, uint32_t slotIdx, uint32_t targetIdx)
    {
        if (!ValidIdx(holderIdx) || !ValidIdx(targetIdx)) {
            return;
        }
        // Ensure mutator TLS is bound — CJ_MCC_StickyLogLine no-ops without it.
        if (Mutator::GetMutator() == nullptr) {
            MutatorManager::Instance().CreateMutator();
        }
        RefField<>& f = *SlotField(slots[holderIdx].obj, slotIdx);
        Heap::GetBarrier().WriteReference(slots[holderIdx].obj, f, slots[targetIdx].obj);
        slots[holderIdx].lastStoreWasBarrier = true;
        slots[holderIdx].lastStoreWasPlain = false;
        ++storeCount;
        ++barrierStoreCount;
    }

    void Drop(uint32_t idx)
    {
        if (!ValidIdx(idx)) {
            return;
        }
        if (slots[idx].pinned && slots[idx].home != nullptr) {
            slots[idx].home->DecRawPointerObjectCount();
        }
        RefField<>* f = SlotField(slots[idx].obj, 0);
        f->SetTargetObject(nullptr);
        slots[idx].live = false;
        slots[idx].obj = nullptr;
    }

    void Pin(uint32_t idx)
    {
        if (!ValidIdx(idx) || slots[idx].pinned) {
            return;
        }
        slots[idx].home->IncRawPointerObjectCount();
        slots[idx].pinned = true;
    }

    void Unpin(uint32_t idx)
    {
        if (!ValidIdx(idx) || !slots[idx].pinned) {
            return;
        }
        slots[idx].home->DecRawPointerObjectCount();
        slots[idx].pinned = false;
    }

    void PromoteAll()
    {
        manager->PromoteAllRegions();
    }

    void ForceMinor()
    {
        Heap::GetHeap().GetCollector().RequestGC(GC_REASON_YOUNG, false);
    }

    void ForceMajor()
    {
        Heap::GetHeap().GetCollector().RequestGC(GC_REASON_USER, false);
    }

    void Grow(uint32_t bytes)
    {
        uint32_t n = bytes / 64u;
        if (n == 0) {
            n = 1;
        }
        if (n > 4096) {
            n = 4096;
        }
        for (uint32_t i = 0; i < n; ++i) {
            (void)AllocSlot(0, static_cast<uint32_t>(AllocKind::MOVEABLE));
        }
    }

    // Pack the region that currently holds slots[objIdx] until Alloc fails
    // (region becomes full / RECENT_FULL candidate). Extra objects go into table.
    void FillRegionOf(uint32_t objIdx)
    {
        if (!ValidIdx(objIdx)) {
            return;
        }
        RegionInfo* reg = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(slots[objIdx].obj));
        if (reg == nullptr) {
            return;
        }
        g_holderTi->SetInstanceSize(8);
        size_t objSize = 8 + TYPEINFO_PTR_SIZE;
        objSize = (objSize + 7u) & ~size_t(7u);
        for (int i = 0; i < 4096; ++i) {
            MAddress addr = reg->Alloc(objSize);
            if (addr == 0) {
                break;
            }
            BaseObject* obj = reinterpret_cast<BaseObject*>(addr);
            obj->SetClassInfo(g_holderTi);
            auto* slot0 = reinterpret_cast<RefField<>*>(reinterpret_cast<uintptr_t>(obj) + TYPEINFO_PTR_SIZE);
            slot0->SetTargetObject(nullptr);
            Slot s;
            s.obj = obj;
            s.home = reg;
            s.live = true;
            s.pinned = false;
            s.size = static_cast<uint32_t>(objSize);
            slots.push_back(s);
            ++allocCount;
        }
    }

    // Independent remset observer: walk live table slots; for each old→young ref,
    // require holder's sticky line to be logged. Report first miss.
    // Barrier-only hit: missing edge + last store was STORE_BARRIER + region pending=0.
    bool ObserveRemset()
    {
        ++observeCount;
        lastObserveHit = false;
        lastBarrierOnlyHit = false;
        lastMissDetail.clear();
        missingEdges = 0;
        loggedEdges = 0;
        oldToYoungEdges = 0;
        barrierMissing = 0;
        plainMissing = 0;
        StickyLog& log = StickyLog::Instance();

        for (size_t i = 0; i < slots.size(); ++i) {
            if (!slots[i].live || slots[i].obj == nullptr) {
                continue;
            }
            BaseObject* holder = slots[i].obj;
            RegionInfo* hReg = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            if (hReg == nullptr || !hReg->IsValidRegion() || hReg->IsGarbageRegion()) {
                continue;
            }
            if (hReg->IsYoungRegion()) {
                continue;
            }
            RefField<>* f = SlotField(holder, 0);
            BaseObject* target = f->GetTargetObject();
            if (target == nullptr || !Heap::IsHeapAddress(reinterpret_cast<MAddress>(target))) {
                continue;
            }
            RegionInfo* tReg = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (tReg == nullptr || !tReg->IsYoungRegion()) {
                continue;
            }
            ++oldToYoungEdges;
            MAddress line = reinterpret_cast<MAddress>(holder);
            bool logged = log.IsLoggedLine(line);
            uint64_t pending = CountPendingLinesInRegion(hReg);
            if (logged) {
                ++loggedEdges;
            } else {
                ++missingEdges;
                if (slots[i].lastStoreWasBarrier) {
                    ++barrierMissing;
                }
                if (slots[i].lastStoreWasPlain) {
                    ++plainMissing;
                }
                // Barrier-only hit: STORE_BARRIER produced the edge, line gone, region clean.
                if (slots[i].lastStoreWasBarrier && pending == 0) {
                    lastBarrierOnlyHit = true;
                }
                if (lastMissDetail.empty()) {
                    char buf[512];
                    std::snprintf(buf, sizeof(buf),
                        "holderIdx=%zu holder=%p target=%p holderYoung=0 targetYoung=1 "
                        "lineLogged=0 holderRegionType=%u targetRegionType=%u "
                        "regionPendingLines=%" PRIu64 " storeBarrier=%d storePlain=%d",
                        i, static_cast<void*>(holder), static_cast<void*>(target),
                        static_cast<unsigned>(hReg->GetRegionType()),
                        static_cast<unsigned>(tReg->GetRegionType()),
                        pending,
                        slots[i].lastStoreWasBarrier ? 1 : 0,
                        slots[i].lastStoreWasPlain ? 1 : 0);
                    lastMissDetail = buf;
                    lastMiss.holderIdx = i;
                    lastMiss.holder = holder;
                    lastMiss.target = target;
                    lastMiss.holderRegionType = static_cast<unsigned>(hReg->GetRegionType());
                    lastMiss.targetRegionType = static_cast<unsigned>(tReg->GetRegionType());
                    lastMiss.regionPendingLines = pending;
                    lastMiss.storeWasBarrier = slots[i].lastStoreWasBarrier;
                    lastMiss.storeWasPlain = slots[i].lastStoreWasPlain;
                    lastMiss.lineLogged = false;
                }
            }
        }

        std::printf("[GCDRIVER-OBSERVE] oldToYoung=%" PRIu64 " logged=%" PRIu64
                    " missing=%" PRIu64 " barrierMissing=%" PRIu64 " plainMissing=%" PRIu64
                    " barrierOnlyHit=%d\n",
            oldToYoungEdges, loggedEdges, missingEdges, barrierMissing, plainMissing,
            lastBarrierOnlyHit ? 1 : 0);
        if (missingEdges > 0) {
            lastObserveHit = true;
            std::printf("[GCDRIVER-MISS] %s\n", lastMissDetail.c_str());
        }
        std::fflush(stdout);
        return requireBarrierOnlyHit ? lastBarrierOnlyHit : lastObserveHit;
    }

    bool RunEvent(const Event& e)
    {
        ++stepIndex;
        bool ok = true;
        switch (e.op) {
            case Op::ALLOC:
                ok = AllocSlot(e.a, e.b) >= 0;
                break;
            case Op::STORE:
            case Op::STORE_PLAIN:
                StorePlain(e.a, e.b, e.c);
                break;
            case Op::STORE_BARRIER:
                StoreBarrier(e.a, e.b, e.c);
                break;
            case Op::DROP:
                Drop(e.a);
                break;
            case Op::FORCE_MINOR:
                ForceMinor();
                break;
            case Op::FORCE_MAJOR:
                ForceMajor();
                break;
            case Op::GROW:
                Grow(e.a);
                break;
            case Op::PIN:
                Pin(e.a);
                break;
            case Op::UNPIN:
                Unpin(e.a);
                break;
            case Op::OBSERVE:
                (void)ObserveRemset();
                break;
            case Op::PROMOTE:
                PromoteAll();
                break;
            case Op::FILL_REGION:
                FillRegionOf(e.a);
                break;
            default:
                return false;
        }
        if (trackStepTrace && e.op != Op::OBSERVE) {
            // Lightweight step snapshot for culprit localization (no full observe).
            StickyLog& log = StickyLog::Instance();
            uint64_t o2y = 0, miss = 0, blog = 0;
            for (size_t i = 0; i < slots.size(); ++i) {
                if (!slots[i].live || slots[i].obj == nullptr) {
                    continue;
                }
                RegionInfo* hReg = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(slots[i].obj));
                if (hReg == nullptr || hReg->IsYoungRegion()) {
                    continue;
                }
                BaseObject* t = SlotField(slots[i].obj, 0)->GetTargetObject();
                if (t == nullptr || !Heap::IsHeapAddress(reinterpret_cast<MAddress>(t))) {
                    continue;
                }
                RegionInfo* tReg = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(t));
                if (tReg == nullptr || !tReg->IsYoungRegion()) {
                    continue;
                }
                ++o2y;
                if (!log.IsLoggedLine(reinterpret_cast<MAddress>(slots[i].obj))) {
                    ++miss;
                    if (slots[i].lastStoreWasBarrier) {
                        ++blog;
                    }
                }
            }
            std::printf("[GCDRIVER-STEP] i=%" PRIu64 " op=%u a=%u b=%u c=%u o2y=%" PRIu64
                        " miss=%" PRIu64 " barrierMiss=%" PRIu64 "\n",
                stepIndex, static_cast<unsigned>(e.op), e.a, e.b, e.c, o2y, miss, blog);
        }
        return ok;
    }

    void RunAll(const std::vector<Event>& events)
    {
        for (const Event& e : events) {
            (void)RunEvent(e);
        }
    }
};

// ---- encode / decode ----
const char* OpName(Op op)
{
    switch (op) {
        case Op::ALLOC: return "ALLOC";
        case Op::STORE: return "STORE";
        case Op::DROP: return "DROP";
        case Op::FORCE_MINOR: return "FORCE_MINOR";
        case Op::FORCE_MAJOR: return "FORCE_MAJOR";
        case Op::GROW: return "GROW";
        case Op::PIN: return "PIN";
        case Op::UNPIN: return "UNPIN";
        case Op::OBSERVE: return "OBSERVE";
        case Op::PROMOTE: return "PROMOTE";
        case Op::STORE_PLAIN: return "STORE_PLAIN";
        case Op::STORE_BARRIER: return "STORE_BARRIER";
        case Op::FILL_REGION: return "FILL_REGION";
        default: return "UNKNOWN";
    }
}

bool ParseOp(const char* s, Op& out)
{
    if (std::strcmp(s, "ALLOC") == 0) { out = Op::ALLOC; return true; }
    if (std::strcmp(s, "STORE") == 0) { out = Op::STORE; return true; }
    if (std::strcmp(s, "DROP") == 0) { out = Op::DROP; return true; }
    if (std::strcmp(s, "FORCE_MINOR") == 0) { out = Op::FORCE_MINOR; return true; }
    if (std::strcmp(s, "FORCE_MAJOR") == 0) { out = Op::FORCE_MAJOR; return true; }
    if (std::strcmp(s, "GROW") == 0) { out = Op::GROW; return true; }
    if (std::strcmp(s, "PIN") == 0) { out = Op::PIN; return true; }
    if (std::strcmp(s, "UNPIN") == 0) { out = Op::UNPIN; return true; }
    if (std::strcmp(s, "OBSERVE") == 0) { out = Op::OBSERVE; return true; }
    if (std::strcmp(s, "PROMOTE") == 0) { out = Op::PROMOTE; return true; }
    if (std::strcmp(s, "STORE_PLAIN") == 0) { out = Op::STORE_PLAIN; return true; }
    if (std::strcmp(s, "STORE_BARRIER") == 0) { out = Op::STORE_BARRIER; return true; }
    if (std::strcmp(s, "FILL_REGION") == 0) { out = Op::FILL_REGION; return true; }
    return false;
}

void WriteEvents(const char* path, const std::vector<Event>& events)
{
    FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        std::printf("GCDRIVER write FAIL %s errno=%d\n", path, errno);
        return;
    }
    for (const Event& e : events) {
        std::fprintf(f, "%s %u %u %u\n", OpName(e.op), e.a, e.b, e.c);
    }
    std::fclose(f);
}

bool ReadEvents(const char* path, std::vector<Event>& events)
{
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) {
        return false;
    }
    char name[64];
    unsigned a, b, c;
    while (std::fscanf(f, "%63s %u %u %u", name, &a, &b, &c) == 4) {
        Op op;
        if (!ParseOp(name, op)) {
            std::fclose(f);
            return false;
        }
        events.push_back(Event{ op, a, b, c });
    }
    std::fclose(f);
    return true;
}

// Positive-control shape: STORE_PLAIN old→young (observer must catch).
std::vector<Event> SequenceOldYoungUnlogged()
{
    return {
        { Op::ALLOC, 0, 0, 0 },
        { Op::ALLOC, 0, 0, 0 },
        { Op::PIN, 0, 0, 0 },
        { Op::PIN, 1, 0, 0 },
        { Op::PROMOTE, 0, 0, 0 },
        { Op::ALLOC, 0, 0, 0 },
        { Op::PIN, 2, 0, 0 },
        { Op::STORE_PLAIN, 0, 0, 2 },
        { Op::OBSERVE, 0, 0, 0 },
    };
}

// Barrier baseline: same shape with STORE_BARRIER — expect missing=0 if log works.
std::vector<Event> SequenceOldYoungBarrier()
{
    return {
        { Op::ALLOC, 0, 0, 0 },
        { Op::ALLOC, 0, 0, 0 },
        { Op::PIN, 0, 0, 0 },
        { Op::PIN, 1, 0, 0 },
        { Op::PROMOTE, 0, 0, 0 },
        { Op::ALLOC, 0, 0, 0 },
        { Op::PIN, 2, 0, 0 },
        { Op::STORE_BARRIER, 0, 0, 2 },
        { Op::OBSERVE, 0, 0, 0 },
    };
}

// Hypothesis sequences (barrier-only).
// H1: barrier write → minor (consume) → barrier write again → observe
std::vector<Event> SequenceH1WriteMinorRewrite()
{
    return {
        { Op::ALLOC, 0, 0, 0 }, // 0 holder
        { Op::ALLOC, 0, 0, 0 }, // 1 spare
        { Op::PIN, 0, 0, 0 },
        { Op::PROMOTE, 0, 0, 0 },
        { Op::ALLOC, 0, 0, 0 }, // 2 young target
        { Op::PIN, 2, 0, 0 },
        { Op::STORE_BARRIER, 0, 0, 2 },
        { Op::FORCE_MINOR, 0, 0, 0 },
        { Op::ALLOC, 0, 0, 0 }, // 3 new young
        { Op::PIN, 3, 0, 0 },
        { Op::STORE_BARRIER, 0, 0, 3 },
        { Op::OBSERVE, 0, 0, 0 },
    };
}

// H2: barrier write → major (BeginEpoch clears map) → observe without re-write
std::vector<Event> SequenceH2WriteMajorNoRewrite()
{
    return {
        { Op::ALLOC, 0, 0, 0 },
        { Op::PIN, 0, 0, 0 },
        { Op::PROMOTE, 0, 0, 0 },
        { Op::ALLOC, 0, 0, 0 }, // 1 young
        { Op::PIN, 1, 0, 0 },
        { Op::STORE_BARRIER, 0, 0, 1 },
        { Op::FORCE_MAJOR, 0, 0, 0 },
        { Op::OBSERVE, 0, 0, 0 },
    };
}

// H3: fill holder region → promote → barrier store young → observe
std::vector<Event> SequenceH3FillPromoteStore()
{
    return {
        { Op::ALLOC, 0, 0, 0 }, // 0
        { Op::FILL_REGION, 0, 0, 0 },
        { Op::PIN, 0, 0, 0 },
        { Op::PROMOTE, 0, 0, 0 },
        { Op::ALLOC, 0, 0, 0 }, // young target (new region)
        { Op::PIN, 0, 0, 0 },   // re-pin holder if still live idx0
        { Op::STORE_BARRIER, 0, 0, 1 },
        { Op::OBSERVE, 0, 0, 0 },
    };
}

// H4: barrier → minor → major → minor interleaved
std::vector<Event> SequenceH4MajorMinorInterleave()
{
    return {
        { Op::ALLOC, 0, 0, 0 },
        { Op::PIN, 0, 0, 0 },
        { Op::PROMOTE, 0, 0, 0 },
        { Op::ALLOC, 0, 0, 0 },
        { Op::PIN, 1, 0, 0 },
        { Op::STORE_BARRIER, 0, 0, 1 },
        { Op::FORCE_MAJOR, 0, 0, 0 },
        { Op::FORCE_MINOR, 0, 0, 0 },
        { Op::OBSERVE, 0, 0, 0 },
    };
}

// H5: barrier write, then FILL other regions / GROW, promote path via age, observe
std::vector<Event> SequenceH5GrowAround()
{
    return {
        { Op::ALLOC, 0, 0, 0 },
        { Op::PIN, 0, 0, 0 },
        { Op::PROMOTE, 0, 0, 0 },
        { Op::ALLOC, 0, 0, 0 },
        { Op::PIN, 1, 0, 0 },
        { Op::STORE_BARRIER, 0, 0, 1 },
        { Op::GROW, 8192, 0, 0 },
        { Op::FORCE_MINOR, 0, 0, 0 },
        { Op::OBSERVE, 0, 0, 0 },
    };
}

// Seeded random sequence. barrierOnly=true ⇒ never emits STORE_PLAIN.
std::vector<Event> GenerateFromSeed(uint64_t seed, uint32_t n, bool barrierOnly)
{
    Rng rng(seed);
    std::vector<Event> out;
    out.reserve(n + 4);
    out.push_back({ Op::ALLOC, 0, 0, 0 });
    out.push_back({ Op::ALLOC, 0, 0, 0 });
    uint32_t live = 2;
    for (uint32_t i = 0; i + 1 < n; ++i) {
        uint32_t pick = rng.next_u32(0, barrierOnly ? 11 : 12);
        Event e {};
        switch (pick) {
            case 0:
            case 1:
                e = { Op::ALLOC, rng.next_u32(0, 3), rng.next_u32(0, 1), 0 };
                ++live;
                break;
            case 2:
            case 3:
                if (barrierOnly) {
                    e = { Op::STORE_BARRIER, rng.next_u32(0, live > 0 ? live - 1 : 0), 0,
                        rng.next_u32(0, live > 0 ? live - 1 : 0) };
                } else {
                    e = { Op::STORE_PLAIN, rng.next_u32(0, live > 0 ? live - 1 : 0), 0,
                        rng.next_u32(0, live > 0 ? live - 1 : 0) };
                }
                break;
            case 4:
            case 5:
                e = { Op::STORE_BARRIER, rng.next_u32(0, live > 0 ? live - 1 : 0), 0,
                    rng.next_u32(0, live > 0 ? live - 1 : 0) };
                break;
            case 6:
                e = { Op::PROMOTE, 0, 0, 0 };
                break;
            case 7:
                e = { Op::PIN, rng.next_u32(0, live > 0 ? live - 1 : 0), 0, 0 };
                break;
            case 8:
                e = { Op::DROP, rng.next_u32(0, live > 0 ? live - 1 : 0), 0, 0 };
                break;
            case 9:
                e = { Op::GROW, rng.next_u32(64, 8192), 0, 0 };
                break;
            case 10:
                e = { Op::FORCE_MINOR, 0, 0, 0 };
                break;
            case 11:
                e = { Op::FORCE_MAJOR, 0, 0, 0 };
                break;
            default:
                e = { Op::FILL_REGION, rng.next_u32(0, live > 0 ? live - 1 : 0), 0, 0 };
                break;
        }
        out.push_back(e);
    }
    out.push_back({ Op::OBSERVE, 0, 0, 0 });
    return out;
}

bool RunSequenceInProcess(const std::vector<Event>& events, Driver& d)
{
    d.slots.clear();
    d.ResetStats();
    d.RunAll(events);
    return d.requireBarrierOnlyHit ? d.lastBarrierOnlyHit : d.lastObserveHit;
}

// Naive linear reduction: drop one event at a time while predicate holds.
std::vector<Event> Reduce(const std::vector<Event>& input, bool (*predicate)(const std::vector<Event>&))
{
    std::vector<Event> cur = input;
    bool improved = true;
    while (improved && cur.size() > 1) {
        improved = false;
        for (size_t i = 0; i < cur.size(); ++i) {
            if (cur[i].op == Op::OBSERVE && i + 1 == cur.size()) {
                continue;
            }
            std::vector<Event> cand;
            cand.reserve(cur.size() - 1);
            for (size_t j = 0; j < cur.size(); ++j) {
                if (j != i) {
                    cand.push_back(cur[j]);
                }
            }
            if (predicate(cand)) {
                cur.swap(cand);
                improved = true;
                break;
            }
        }
    }
    return cur;
}

// True if sequence contains any STORE_PLAIN (excluded from barrier-only hits).
bool SequenceHasStorePlain(const std::vector<Event>& events)
{
    for (const Event& e : events) {
        if (e.op == Op::STORE_PLAIN || e.op == Op::STORE) {
            return true;
        }
    }
    return false;
}

void PrintUsage()
{
    std::printf(
        "gc_driver — synthetic event-sequence driver for the memory manager\n"
        "  --seed <n>           generate deterministic sequence from seed\n"
        "  --events <n>         event count for --seed (default 32)\n"
        "  --replay <file>      replay event file\n"
        "  --write <file>       write generated/replayed sequence\n"
        "  --shape oldyoung     positive-control STORE_PLAIN old→young\n"
        "  --shape barrier      same shape with STORE_BARRIER (expect logged)\n"
        "  --shape h1|h2|h3|h4|h5  hypothesis sequences (barrier-only)\n"
        "  --barrier-only       seed gen never emits STORE_PLAIN; hit=barrierOnlyHit\n"
        "  --scan-seeds <n>     try seeds 1..n (or --seed base) for barrier-only hit\n"
        "  --step-trace         print per-event o2y/miss snapshot\n"
        "  --reduce             after a hit, reduce to a minimal subset\n"
        "  --dump               print events to stdout\n"
        "  --no-fini            skip FiniAndDelete (diagnose rc=134)\n");
}

} // namespace
} // namespace MapleRuntime

static MapleRuntime::Driver* g_driver = nullptr;
static bool ReducePredicate(const std::vector<MapleRuntime::Event>& events)
{
    using namespace MapleRuntime;
    if (g_driver == nullptr) {
        return false;
    }
    if (g_driver->requireBarrierOnlyHit && SequenceHasStorePlain(events)) {
        return false;
    }
    g_driver->slots.clear();
    g_driver->ResetStats();
    g_driver->RunAll(events);
    return g_driver->requireBarrierOnlyHit ? g_driver->lastBarrierOnlyHit : g_driver->lastObserveHit;
}

int main(int argc, char** argv)
{
    using namespace MapleRuntime;

    uint64_t seed = 0;
    bool haveSeed = false;
    uint32_t nEvents = 32;
    const char* replayPath = nullptr;
    const char* writePath = nullptr;
    const char* shapeName = nullptr;
    bool doReduce = false;
    bool doDump = false;
    bool barrierOnly = false;
    uint32_t scanSeeds = 0;
    bool stepTrace = false;
    bool noFini = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 0);
            haveSeed = true;
        } else if (std::strcmp(argv[i], "--events") == 0 && i + 1 < argc) {
            nEvents = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            replayPath = argv[++i];
        } else if (std::strcmp(argv[i], "--write") == 0 && i + 1 < argc) {
            writePath = argv[++i];
        } else if (std::strcmp(argv[i], "--shape") == 0 && i + 1 < argc) {
            shapeName = argv[++i];
        } else if (std::strcmp(argv[i], "--reduce") == 0) {
            doReduce = true;
        } else if (std::strcmp(argv[i], "--dump") == 0) {
            doDump = true;
        } else if (std::strcmp(argv[i], "--barrier-only") == 0) {
            barrierOnly = true;
        } else if (std::strcmp(argv[i], "--scan-seeds") == 0 && i + 1 < argc) {
            scanSeeds = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
            barrierOnly = true;
        } else if (std::strcmp(argv[i], "--step-trace") == 0) {
            stepTrace = true;
        } else if (std::strcmp(argv[i], "--no-fini") == 0) {
            noFini = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            PrintUsage();
            return 0;
        }
    }

    setenv("MRT_STICKY_MINOR", "1", 1);
    setenv("MRT_STICKY_MINOR_FORCE_SLOW_PATH", "1", 1);

    MRT_CjRuntimeInit();
    InitSyntheticTypeInfo();

    // Bind a mutator so IdleLogBarrier → CJ_MCC_StickyLogLine actually logs.
    Mutator* mut = MutatorManager::Instance().CreateMutator();
    std::printf("GCDRIVER mutator=%p\n", static_cast<void*>(mut));

    auto& allocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& manager = allocator.GetRegionManager();

    Driver driver;
    driver.manager = &manager;
    driver.requireBarrierOnlyHit = barrierOnly;
    driver.trackStepTrace = stepTrace;
    g_driver = &driver;

    auto runOne = [&](const std::vector<Event>& events) -> bool {
        return RunSequenceInProcess(events, driver);
    };

    // ---- multi-seed scan mode ----
    if (scanSeeds > 0) {
        uint64_t base = haveSeed ? seed : 1;
        uint64_t hitSeed = 0;
        std::vector<Event> hitEvents;
        uint32_t tried = 0;
        for (uint32_t k = 0; k < scanSeeds; ++k) {
            uint64_t s = base + k;
            std::vector<Event> events = GenerateFromSeed(s, nEvents, true);
            ++tried;
            if (SequenceHasStorePlain(events)) {
                continue; // should not happen in barrierOnly gen
            }
            bool hit = runOne(events);
            if ((k & 63u) == 0u) {
                std::printf("GCDRIVER SCAN progress k=%u seed=%" PRIu64 " hit=%d barrierMissing=%" PRIu64 "\n",
                    k, s, hit ? 1 : 0, driver.barrierMissing);
                std::fflush(stdout);
            }
            if (hit) {
                hitSeed = s;
                hitEvents = events;
                std::printf("GCDRIVER SCAN_HIT seed=%" PRIu64 " tried=%u events=%zu\n", s, tried, events.size());
                break;
            }
        }
        if (hitSeed != 0) {
            if (writePath != nullptr) {
                WriteEvents(writePath, hitEvents);
            }
            if (doReduce) {
                std::vector<Event> reduced = Reduce(hitEvents, ReducePredicate);
                std::printf("GCDRIVER REDUCED from=%zu to=%zu\n", hitEvents.size(), reduced.size());
                if (writePath != nullptr) {
                    std::string rpath = std::string(writePath) + ".reduced";
                    WriteEvents(rpath.c_str(), reduced);
                }
                for (const Event& e : reduced) {
                    std::printf("REDUCED %s %u %u %u\n", OpName(e.op), e.a, e.b, e.c);
                }
                driver.trackStepTrace = true;
                const bool hit2 = runOne(reduced);
                std::printf("GCDRIVER REDUCED_CONFIRM hit=%d barrierOnlyHit=%d missing=%" PRIu64
                            " barrierMissing=%" PRIu64 "\n",
                    hit2 ? 1 : 0, driver.lastBarrierOnlyHit ? 1 : 0, driver.missingEdges, driver.barrierMissing);
            }
            std::printf("GCDRIVER_DONE hit=1 barrierOnly=1 seed=%" PRIu64 " tried=%u\n", hitSeed, tried);
            if (!noFini) {
                CangjieRuntime::FiniAndDelete();
            }
            return 0;
        }
        std::printf("GCDRIVER_DONE hit=0 barrierOnly=1 tried=%u\n", tried);
        if (!noFini) {
            CangjieRuntime::FiniAndDelete();
        }
        return 1;
    }

    // ---- single sequence mode ----
    std::vector<Event> events;
    if (replayPath != nullptr) {
        if (!ReadEvents(replayPath, events)) {
            std::printf("GCDRIVER replay FAIL path=%s\n", replayPath);
            return 2;
        }
    } else if (shapeName != nullptr) {
        if (std::strcmp(shapeName, "oldyoung") == 0) {
            events = SequenceOldYoungUnlogged();
        } else if (std::strcmp(shapeName, "barrier") == 0) {
            events = SequenceOldYoungBarrier();
        } else if (std::strcmp(shapeName, "h1") == 0) {
            events = SequenceH1WriteMinorRewrite();
            barrierOnly = true;
            driver.requireBarrierOnlyHit = true;
        } else if (std::strcmp(shapeName, "h2") == 0) {
            events = SequenceH2WriteMajorNoRewrite();
            barrierOnly = true;
            driver.requireBarrierOnlyHit = true;
        } else if (std::strcmp(shapeName, "h3") == 0) {
            events = SequenceH3FillPromoteStore();
            barrierOnly = true;
            driver.requireBarrierOnlyHit = true;
        } else if (std::strcmp(shapeName, "h4") == 0) {
            events = SequenceH4MajorMinorInterleave();
            barrierOnly = true;
            driver.requireBarrierOnlyHit = true;
        } else if (std::strcmp(shapeName, "h5") == 0) {
            events = SequenceH5GrowAround();
            barrierOnly = true;
            driver.requireBarrierOnlyHit = true;
        } else {
            std::printf("GCDRIVER unknown shape=%s\n", shapeName);
            return 2;
        }
    } else if (haveSeed) {
        events = GenerateFromSeed(seed, nEvents, barrierOnly);
    } else {
        events = SequenceOldYoungUnlogged();
    }

    if (doDump) {
        for (const Event& e : events) {
            std::printf("%s %u %u %u\n", OpName(e.op), e.a, e.b, e.c);
        }
    }
    if (writePath != nullptr) {
        WriteEvents(writePath, events);
    }

    const bool hit = runOne(events);

    std::printf("GCDRIVER events=%zu allocs=%" PRIu64 " stores=%" PRIu64
                " barrierStores=%" PRIu64 " plainStores=%" PRIu64
                " observes=%" PRIu64 " oldToYoung=%" PRIu64 " missing=%" PRIu64
                " logged=%" PRIu64 " barrierMissing=%" PRIu64 " plainMissing=%" PRIu64
                " hit=%d barrierOnlyHit=%d\n",
        events.size(), driver.allocCount, driver.storeCount, driver.barrierStoreCount,
        driver.plainStoreCount, driver.observeCount, driver.oldToYoungEdges, driver.missingEdges,
        driver.loggedEdges, driver.barrierMissing, driver.plainMissing,
        hit ? 1 : 0, driver.lastBarrierOnlyHit ? 1 : 0);
    if (driver.lastObserveHit) {
        std::printf("GCDRIVER REPRODUCED shape=old-to-young-unlogged-line %s\n",
            driver.lastMissDetail.c_str());
    }
    if (driver.lastBarrierOnlyHit) {
        std::printf("GCDRIVER BARRIER_ONLY_HIT %s\n", driver.lastMissDetail.c_str());
    }

    std::vector<Event> reduced;
    if (doReduce && hit) {
        reduced = Reduce(events, ReducePredicate);
        std::printf("GCDRIVER REDUCED from=%zu to=%zu\n", events.size(), reduced.size());
        if (writePath != nullptr) {
            std::string rpath = std::string(writePath) + ".reduced";
            WriteEvents(rpath.c_str(), reduced);
            std::printf("GCDRIVER wrote reduced %s\n", rpath.c_str());
        }
        for (const Event& e : reduced) {
            std::printf("REDUCED %s %u %u %u\n", OpName(e.op), e.a, e.b, e.c);
        }
        driver.trackStepTrace = true;
        const bool hit2 = runOne(reduced);
        std::printf("GCDRIVER REDUCED_CONFIRM hit=%d missing=%" PRIu64 " barrierOnlyHit=%d\n",
            hit2 ? 1 : 0, driver.missingEdges, driver.lastBarrierOnlyHit ? 1 : 0);
    }

    if (!noFini) {
        CangjieRuntime::FiniAndDelete();
    }

    std::printf("GCDRIVER_DONE hit=%d barrierOnlyHit=%d events=%zu reduced=%zu\n",
        hit ? 1 : 0, driver.lastBarrierOnlyHit ? 1 : 0, events.size(), reduced.size());
    std::fflush(stdout);
    return hit ? 0 : 1;
}
