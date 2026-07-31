// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// Synthetic event-sequence driver for the memory manager.
// Events drive allocation / store / drop / force collection / pin.
// Deterministic --seed / --replay / --reduce. No compiler involved.

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
    // observation / control (driver-local)
    OBSERVE = 9,     // run remset completeness check; fail => "reproduced"
    PROMOTE = 10,    // promote all young regions to old (synthetic age-out)
    STORE_PLAIN = 11,// store without write-barrier / without sticky log
    STORE_BARRIER = 12, // store via Heap barrier (logs when IdleLogBarrier active)
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
};

struct Driver {
    std::vector<Slot> slots;
    RegionManager* manager = nullptr;
    uint64_t allocCount = 0;
    uint64_t storeCount = 0;
    uint64_t observeCount = 0;
    uint64_t missingEdges = 0;
    uint64_t loggedEdges = 0;
    uint64_t oldToYoungEdges = 0;
    bool lastObserveHit = false;
    std::string lastMissDetail;

    void ResetStats()
    {
        allocCount = storeCount = observeCount = 0;
        missingEdges = loggedEdges = oldToYoungEdges = 0;
        lastObserveHit = false;
        lastMissDetail.clear();
    }

    int AllocSlot(uint32_t sizeClass, uint32_t kind)
    {
        // sizeClass 0..3 => payload sizes; object total = TYPEINFO + instanceSize
        static const uint32_t kPayload[] = { 8, 8, 16, 32 };
        uint32_t payload = kPayload[sizeClass % 4];
        // force our synthetic type (one ref) for STORE shapes
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
            // region full — take another
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
        // zero payload refs
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
        // only slot 0 supported in synthetic type
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
        ++storeCount;
    }

    void StoreBarrier(uint32_t holderIdx, uint32_t slotIdx, uint32_t targetIdx)
    {
        if (!ValidIdx(holderIdx) || !ValidIdx(targetIdx)) {
            return;
        }
        RefField<>& f = *SlotField(slots[holderIdx].obj, slotIdx);
        Heap::GetBarrier().WriteReference(slots[holderIdx].obj, f, slots[targetIdx].obj);
        ++storeCount;
    }

    void Drop(uint32_t idx)
    {
        if (!ValidIdx(idx)) {
            return;
        }
        if (slots[idx].pinned && slots[idx].home != nullptr) {
            slots[idx].home->DecRawPointerObjectCount();
        }
        // clear outgoing refs only; do not free (region reclaim is GC's job)
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

    // Independent remset observer: walk live table slots; for each old→young ref,
    // require holder's sticky line to be logged. Report first miss.
    bool ObserveRemset()
    {
        ++observeCount;
        lastObserveHit = false;
        lastMissDetail.clear();
        missingEdges = 0;
        loggedEdges = 0;
        oldToYoungEdges = 0;
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
                continue; // only old holders produce remset edges
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
            if (logged) {
                ++loggedEdges;
            } else {
                ++missingEdges;
                if (lastMissDetail.empty()) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                        "holderIdx=%zu holder=%p target=%p holderYoung=0 targetYoung=1 "
                        "lineLogged=0 holderRegionType=%u",
                        i, static_cast<void*>(holder), static_cast<void*>(target),
                        static_cast<unsigned>(hReg->GetRegionType()));
                    lastMissDetail = buf;
                }
            }
        }

        std::printf("[GCDRIVER-OBSERVE] oldToYoung=%" PRIu64 " logged=%" PRIu64 " missing=%" PRIu64 "\n",
            oldToYoungEdges, loggedEdges, missingEdges);
        if (missingEdges > 0) {
            lastObserveHit = true;
            std::printf("[GCDRIVER-MISS] %s\n", lastMissDetail.c_str());
        }
        std::fflush(stdout);
        return lastObserveHit;
    }

    bool RunEvent(const Event& e)
    {
        switch (e.op) {
            case Op::ALLOC:
                return AllocSlot(e.a, e.b) >= 0;
            case Op::STORE:
            case Op::STORE_PLAIN:
                StorePlain(e.a, e.b, e.c);
                return true;
            case Op::STORE_BARRIER:
                StoreBarrier(e.a, e.b, e.c);
                return true;
            case Op::DROP:
                Drop(e.a);
                return true;
            case Op::FORCE_MINOR:
                ForceMinor();
                return true;
            case Op::FORCE_MAJOR:
                ForceMajor();
                return true;
            case Op::GROW:
                Grow(e.a);
                return true;
            case Op::PIN:
                Pin(e.a);
                return true;
            case Op::UNPIN:
                Unpin(e.a);
                return true;
            case Op::OBSERVE:
                (void)ObserveRemset();
                return true;
            case Op::PROMOTE:
                PromoteAll();
                return true;
            default:
                return false;
        }
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

// Canonical minimal sequence for old→young unlogged edge.
// Shape: alloc holder, alloc target, promote holder region to old, plain store, observe.
std::vector<Event> SequenceOldYoungUnlogged()
{
    // indices: 0=holder, 1=target
    return {
        { Op::ALLOC, 0, 0, 0 },          // holder (young)
        { Op::ALLOC, 0, 0, 0 },          // target (young)
        { Op::PIN, 0, 0, 0 },            // keep holder across promote/GC
        { Op::PIN, 1, 0, 0 },            // keep target for observation
        { Op::PROMOTE, 0, 0, 0 },        // holder region → old (all young→old)
        // after promote both are old; re-alloc a fresh young target
        { Op::ALLOC, 0, 0, 0 },          // idx 2 young target
        { Op::PIN, 2, 0, 0 },
        { Op::STORE_PLAIN, 0, 0, 2 },    // old[0] → young[2], NO sticky log
        { Op::OBSERVE, 0, 0, 0 },
    };
}

// Seeded random sequence (deterministic). Always ends with OBSERVE.
std::vector<Event> GenerateFromSeed(uint64_t seed, uint32_t n)
{
    Rng rng(seed);
    std::vector<Event> out;
    out.reserve(n + 4);
    // ensure at least two objects and a plain old→young attempt
    out.push_back({ Op::ALLOC, 0, 0, 0 });
    out.push_back({ Op::ALLOC, 0, 0, 0 });
    uint32_t live = 2;
    for (uint32_t i = 0; i + 1 < n; ++i) {
        uint32_t pick = rng.next_u32(0, 9);
        Event e {};
        switch (pick) {
            case 0:
            case 1:
                e = { Op::ALLOC, rng.next_u32(0, 3), rng.next_u32(0, 1), 0 };
                ++live;
                break;
            case 2:
            case 3:
                e = { Op::STORE_PLAIN, rng.next_u32(0, live > 0 ? live - 1 : 0), 0,
                    rng.next_u32(0, live > 0 ? live - 1 : 0) };
                break;
            case 4:
                e = { Op::STORE_BARRIER, rng.next_u32(0, live > 0 ? live - 1 : 0), 0,
                    rng.next_u32(0, live > 0 ? live - 1 : 0) };
                break;
            case 5:
                e = { Op::PROMOTE, 0, 0, 0 };
                break;
            case 6:
                e = { Op::PIN, rng.next_u32(0, live > 0 ? live - 1 : 0), 0, 0 };
                break;
            case 7:
                e = { Op::DROP, rng.next_u32(0, live > 0 ? live - 1 : 0), 0, 0 };
                break;
            case 8:
                e = { Op::GROW, rng.next_u32(64, 4096), 0, 0 };
                break;
            default:
                e = { Op::FORCE_MINOR, 0, 0, 0 };
                break;
        }
        out.push_back(e);
    }
    out.push_back({ Op::OBSERVE, 0, 0, 0 });
    return out;
}

// Fresh runtime driver for one sequence run (caller owns process lifetime).
// We re-init only once per process; reduce re-runs reuse heap — for reduce we
// re-exec via --replay of candidate files from the outer reduce loop.
// Here, for in-process reduce, we only DROP all and re-alloc (heap may be dirty).
// Prefer process-level reduce for determinism; provide in-process for speed.

bool RunSequenceInProcess(const std::vector<Event>& events, Driver& d)
{
    d.slots.clear();
    d.ResetStats();
    d.RunAll(events);
    return d.lastObserveHit;
}

// Naive linear reduction: drop one event at a time while predicate holds.
std::vector<Event> Reduce(const std::vector<Event>& input, bool (*predicate)(const std::vector<Event>&))
{
    std::vector<Event> cur = input;
    bool improved = true;
    while (improved && cur.size() > 1) {
        improved = false;
        for (size_t i = 0; i < cur.size(); ++i) {
            // never drop the final OBSERVE if present
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

void PrintUsage()
{
    std::printf(
        "gc_driver — synthetic event-sequence driver for the memory manager\n"
        "  --seed <n>           generate deterministic sequence from seed\n"
        "  --events <n>         event count for --seed (default 32)\n"
        "  --replay <file>      replay event file\n"
        "  --write <file>       write generated/replayed sequence\n"
        "  --shape oldyoung     run the canonical old→young unlogged-edge sequence\n"
        "  --reduce             after a hit, reduce to a minimal subset (re-exec style via temp)\n"
        "  --dump               print events to stdout\n");
}

} // namespace
} // namespace MapleRuntime

// Global driver for reduce predicate (same process; sequence must be self-contained).
static MapleRuntime::Driver* g_driver = nullptr;
static bool ReducePredicate(const std::vector<MapleRuntime::Event>& events)
{
    using namespace MapleRuntime;
    if (g_driver == nullptr) {
        return false;
    }
    // Clear table; objects remain on heap (acceptable for reduction of observation).
    g_driver->slots.clear();
    g_driver->ResetStats();
    g_driver->RunAll(events);
    return g_driver->lastObserveHit;
}

int main(int argc, char** argv)
{
    using namespace MapleRuntime;

    uint64_t seed = 0;
    bool haveSeed = false;
    uint32_t nEvents = 32;
    const char* replayPath = nullptr;
    const char* writePath = nullptr;
    bool shapeOldYoung = false;
    bool doReduce = false;
    bool doDump = false;

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
            if (std::strcmp(argv[++i], "oldyoung") == 0) {
                shapeOldYoung = true;
            }
        } else if (std::strcmp(argv[i], "--reduce") == 0) {
            doReduce = true;
        } else if (std::strcmp(argv[i], "--dump") == 0) {
            doDump = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            PrintUsage();
            return 0;
        }
    }

    if (!haveSeed && replayPath == nullptr && !shapeOldYoung) {
        shapeOldYoung = true; // default: the known shape
    }

    std::vector<Event> events;
    if (replayPath != nullptr) {
        if (!ReadEvents(replayPath, events)) {
            std::printf("GCDRIVER replay FAIL path=%s\n", replayPath);
            return 2;
        }
    } else if (shapeOldYoung) {
        events = SequenceOldYoungUnlogged();
    } else {
        events = GenerateFromSeed(seed, nEvents);
    }

    if (doDump) {
        for (const Event& e : events) {
            std::printf("%s %u %u %u\n", OpName(e.op), e.a, e.b, e.c);
        }
    }
    if (writePath != nullptr) {
        WriteEvents(writePath, events);
    }

    // Force sticky minor on for observation of the log map (even without compiler consumer).
    // StickyLog::ConfigureMinorFromEnvironment runs at init; set env before Init.
    setenv("MRT_STICKY_MINOR", "1", 1);
    setenv("MRT_STICKY_MINOR_FORCE_SLOW_PATH", "1", 1);

    MRT_CjRuntimeInit();
    InitSyntheticTypeInfo();

    auto& allocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& manager = allocator.GetRegionManager();

    Driver driver;
    driver.manager = &manager;
    g_driver = &driver;

    const bool hit = RunSequenceInProcess(events, driver);

    std::printf("GCDRIVER events=%zu allocs=%" PRIu64 " stores=%" PRIu64
                " observes=%" PRIu64 " oldToYoung=%" PRIu64 " missing=%" PRIu64
                " logged=%" PRIu64 " hit=%d\n",
        events.size(), driver.allocCount, driver.storeCount, driver.observeCount,
        driver.oldToYoungEdges, driver.missingEdges, driver.loggedEdges, hit ? 1 : 0);
    if (hit) {
        std::printf("GCDRIVER REPRODUCED shape=old-to-young-unlogged-line %s\n",
            driver.lastMissDetail.c_str());
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
        // re-run reduced for confirmation
        const bool hit2 = RunSequenceInProcess(reduced, driver);
        std::printf("GCDRIVER REDUCED_CONFIRM hit=%d missing=%" PRIu64 "\n",
            hit2 ? 1 : 0, driver.missingEdges);
    }

    CangjieRuntime::FiniAndDelete();

    std::printf("GCDRIVER_DONE hit=%d events=%zu reduced=%zu\n",
        hit ? 1 : 0, events.size(), reduced.size());
    std::fflush(stdout);
    return hit ? 0 : 1;
}
