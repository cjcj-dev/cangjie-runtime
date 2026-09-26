// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
//
// Bounded-end assertion for the mutator stack-overflow recovery cycle.
//
// Mechanism under test: ExceptionManager::StackOverflow (ExceptionManager.cpp:113)
// expands the guard through Mutator::StackGuardExpand (Mutator.cpp:394) into
// CJThreadStackGuardExpand (cjthread.cpp:1885) and the clearer that ends the throw
// recovers it through CJThreadStackGuardRecover (cjthread.cpp:1920). The re-entrant
// recovery cycle reaches the expand again before any clearer has run, so the expand
// has to be a bounded state transition: HotSpot records the guard state as a state
// (runtime/stackOverflow.hpp:41-45) and reguard_stack returns early when the state is
// not one of the two disabled states (runtime/stackOverflow.cpp:220-223), instead of
// stepping the boundary again.
//
// This probe calls the product's own exported CJThread API on the cjthread the managed
// fixture is running on. It compiles no product source and adds no product export; the
// observed value is the product's stack guard, read back with the product getter.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "Common/Runtime.h"
#include "Common/BaseObject.h"
#include "Concurrency/ConcurrencyModel.h"
#include "ExceptionManager.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"

// The concurrency model the product's own guard path reads
// (Mutator::StackGuardExpand asks GetStackGuardCheckFlag, Mutator.cpp:406). The
// abstract base has no other implementation of it, so the fixture supplies the
// one the default configuration runs with: guard pages checked, and the
// configured reserved size the model reports for the runtime-thread branch.
class FixtureConcurrencyModel final : public MapleRuntime::ConcurrencyModel
{
public:
    void VisitGCRoots(MapleRuntime::RootVisitor*) override {}
    size_t GetReservedStackSize() const override { return 0; }
    bool GetStackGuardCheckFlag() const override { return true; }
};

// The product's own scheduler entry, called the way CJThreadModel::Init calls it
// (CJThreadModel.cpp:197-216): attribute init, the reserved-stack set that must happen
// before the first stack exists, then ScheduleNew (schedule.cpp:562), which binds the
// calling thread to cjthread0 and gives that cjthread an allocated stack of its own.
// Every guard read and move below therefore happens on a cjthread that owns a stack, the
// shape the cangjie-runtime#1167 core was taken on, with no test-only product entry.
// The scheduler entry reads the process-wide runtime handle, so publish a minimal one
// first. This is the same shape runtime/tests/signal_stack_guard_preinit.cpp uses; no
// product source is recompiled into this executable and no product export is added.
class PublishedRuntime final : public MapleRuntime::Runtime {
public:
    PublishedRuntime()
    {
        runtime = this;
        // The managers the recovery path reaches for: the exception manager that owns
        // the raiser ThrowImplicitException dispatches through (ExceptionManager.cpp:363)
        // and the concurrency model StackGuardExpand asks about the guard page. Both are
        // runtime singletons the product fills in at startup; the fixture supplies the
        // two this path reads and nothing else.
        exceptionManager = new MapleRuntime::ExceptionManager();
        concurrencyModel = new FixtureConcurrencyModel();
        mutatorManager = new MapleRuntime::MutatorManager();
    }
    ~PublishedRuntime() { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return {}; }
    void SetGCThreshold(uint64_t) override {}
};

// The scheduler calls the registered TLS hook when it hands a cjthread the runtime
// thread-local block (schedule.h:1265 ScheduleGetTlsHookRegister). The product
// registers MRT_GetThreadLocalData for it, with the same cast the product's own
// registration uses (CJThreadModel.cpp:203, CangjieRuntime.cpp:237); the function
// itself is declared by RuntimeConfig.h:53.

struct ScheduleAttr;
extern "C" {
int CJ_ScheduleGetTlsHookRegister(uintptr_t *(*func)(void));
int CJ_ScheduleAttrInit(struct ScheduleAttr *usrAttr);
int CJ_ScheduleAttrStackProtectSet(struct ScheduleAttr *usrAttr, bool open);
int CJ_ScheduleAttrStackGrowSet(struct ScheduleAttr *usrAttr, bool open);
void *CJ_ScheduleNew(int scheduleType, const struct ScheduleAttr *userAttr);
}

extern "C" {
// Exported cjthread ABI (schedule.h:1049/1056/1130/1124/1035, renamed by
// schedule_rename.h:211-222). Declared with the exported names so this probe links
// the product SO directly instead of resolving a test-only alias.
void CJ_CJThreadStackGuardExpand(void);
void CJ_CJThreadStackGuardRecover(void);
void *CJ_CJThreadStackGuardGet(void);
void *CJ_CJThreadStackAddrGet(void);
uintptr_t CJ_CJThreadStackReversedGet(void);
}

namespace {

// The re-entrant cycle that produced cangjie-runtime#1167 ran thousands of turns; the
// bounded form must stop after one. 64 turns separate "bounded" from "one step per
// turn" without depending on the exact number the core happened to record.
constexpr int kTurns = 64;

// The scheduler this probe creates owns process-global teardown that a bare
// standalone binary does not have the services for (the runtime normally finalizes the
// mutator manager first). The measured state is already reported at this point, so leave
// through _exit rather than run static destructors the product never reaches here.
[[noreturn]] void Finish(int rc)
{
    std::fflush(nullptr);
    _exit(rc);
}

void Report(const char *name, int ok)
{
    std::fprintf(stderr, "SO_REENTRY_%s_RESULT ok=%d\n", name, ok);
}

void *SetupScheduler()
{
    // ScheduleAttr is an opaque blob to the caller; the product reads it through
    // ScheduleAttrCheck, so the same size the product's own caller uses is enough.
    if (CJ_ScheduleGetTlsHookRegister(reinterpret_cast<uintptr_t *(*)()>(MapleRuntime::MRT_GetThreadLocalData)) != 0) {
        return nullptr;
    }
    alignas(16) unsigned char attr[256] = {};
    if (CJ_ScheduleAttrInit(reinterpret_cast<struct ScheduleAttr *>(attr)) != 0) {
        return nullptr;
    }
    CJ_ScheduleAttrStackProtectSet(reinterpret_cast<struct ScheduleAttr *>(attr), true);
    CJ_ScheduleAttrStackGrowSet(reinterpret_cast<struct ScheduleAttr *>(attr), true);
    return CJ_ScheduleNew(0 /* SCHEDULE_DEFAULT */, reinterpret_cast<const struct ScheduleAttr *>(attr));
}

// Preconditions are reported, never fatal: a missing precondition must not mask the
// target invariant assertion that follows it.
bool Preconditions(const void *guard, const void *stackEnd, uintptr_t reserved)
{
    bool ok = guard != nullptr && stackEnd != nullptr && reserved > 0;
    std::fprintf(stderr,
        "SO_REENTRY_PRECONDITION guard=%p stack_end=%p reserved=%zu owns_stack=%d ok=%d\n",
        guard, stackEnd, static_cast<size_t>(reserved), stackEnd != nullptr ? 1 : 0, ok ? 1 : 0);
    return ok;
}

int ExpandOnce()
{
    void *before = CJ_CJThreadStackGuardGet();
    void *stackEnd = CJ_CJThreadStackAddrGet();
    uintptr_t reserved = CJ_CJThreadStackReversedGet();
    if (!Preconditions(before, stackEnd, reserved)) {
        Report("EXPAND_ONCE", 0);
        return 2;
    }
    CJ_CJThreadStackGuardExpand();
    void *after = CJ_CJThreadStackGuardGet();
    // Target: one expand hands out exactly the one reserved step of headroom.
    intptr_t moved = reinterpret_cast<intptr_t>(before) - reinterpret_cast<intptr_t>(after);
    int ok = moved == static_cast<intptr_t>(reserved);
    std::fprintf(stderr, "SO_REENTRY_EXPAND_ONCE_OK moved=%zd expected=%zu guard_after=%p ok=%d\n",
        static_cast<ssize_t>(moved), static_cast<size_t>(reserved), after, ok);
    CJ_CJThreadStackGuardRecover();
    Report("EXPAND_ONCE", ok);
    return ok ? 0 : 1;
}

int ExpandBounded()
{
    void *before = CJ_CJThreadStackGuardGet();
    void *stackEnd = CJ_CJThreadStackAddrGet();
    uintptr_t reserved = CJ_CJThreadStackReversedGet();
    if (!Preconditions(before, stackEnd, reserved)) {
        Report("EXPAND_BOUNDED", 0);
        return 2;
    }
    CJ_CJThreadStackGuardExpand();
    void *afterFirst = CJ_CJThreadStackGuardGet();
    for (int turn = 1; turn < kTurns; ++turn) {
        CJ_CJThreadStackGuardExpand();
    }
    void *afterAll = CJ_CJThreadStackGuardGet();
    std::fprintf(stderr, "SO_REENTRY_EXPAND_TURNS turns=%d guard_first=%p guard_last=%p\n",
        kTurns, afterFirst, afterAll);
    // Target: the re-entrant cycle is bounded. Every turn past the first finds the
    // transition already applied and leaves the guard alone, and the guard never
    // reaches below the end of the allocated stack.
    int ok = afterAll == afterFirst &&
             reinterpret_cast<uintptr_t>(afterAll) >= reinterpret_cast<uintptr_t>(stackEnd);
    std::fprintf(stderr,
        "SO_REENTRY_EXPAND_BOUNDED_OK turns=%d stable=%d above_stack_end=%d ok=%d\n",
        kTurns, afterAll == afterFirst ? 1 : 0,
        reinterpret_cast<uintptr_t>(afterAll) >= reinterpret_cast<uintptr_t>(stackEnd) ? 1 : 0, ok);
    CJ_CJThreadStackGuardRecover();
    Report("EXPAND_BOUNDED", ok);
    return ok ? 0 : 1;
}

// The re-entrant cycle itself, constructed rather than waited for.
// ExceptionManager::StackOverflow (ExceptionManager.cpp:154) expands the guard on the way
// in and the clearer that ends the throw recovers it; a re-entry that happens before any
// clearer has run reaches the expand again with the expansion still applied. This case
// performs that unpaired sequence kReentryTurns times over, which is the shape the
// cangjie-runtime#1167 core shows ~1100 times before the wild dispatch, at a count above
// the issue's N>=3000 so the verdict does not depend on how deep a real overflow went.
constexpr int kReentryTurns = 4096;

int ExpandReentry()
{
    void *before = CJ_CJThreadStackGuardGet();
    void *stackEnd = CJ_CJThreadStackAddrGet();
    uintptr_t reserved = CJ_CJThreadStackReversedGet();
    if (!Preconditions(before, stackEnd, reserved)) {
        Report("EXPAND_REENTRY", 0);
        return 2;
    }
    void *afterFirst = nullptr;
    for (int turn = 0; turn < kReentryTurns; ++turn) {
        CJ_CJThreadStackGuardExpand();
        if (turn == 0) {
            afterFirst = CJ_CJThreadStackGuardGet();
        }
    }
    void *afterAll = CJ_CJThreadStackGuardGet();
    intptr_t walked = reinterpret_cast<intptr_t>(afterFirst) - reinterpret_cast<intptr_t>(afterAll);
    std::fprintf(stderr, "SO_REENTRY_EXPAND_TURNS turns=%d guard_first=%p guard_last=%p walked=%zd\n",
        kReentryTurns, afterFirst, afterAll, static_cast<ssize_t>(walked));
    // Target: after N unpaired re-entry turns the guard is still the one the first turn
    // installed, and it is still at or above the end of the allocated stack. Both halves
    // matter: the first is the boundedness of the cycle, the second is that the
    // threshold the product installs still names an address inside the mapping.
    bool stable = afterAll == afterFirst;
    bool inside = reinterpret_cast<uintptr_t>(afterAll) >= reinterpret_cast<uintptr_t>(stackEnd);
    int ok = stable && inside ? 1 : 0;
    std::fprintf(stderr,
        "SO_REENTRY_EXPAND_REENTRY_OK turns=%d stable=%d above_stack_end=%d walked=%zd ok=%d\n",
        kReentryTurns, stable ? 1 : 0, inside ? 1 : 0, static_cast<ssize_t>(walked), ok);
    CJ_CJThreadStackGuardRecover();
    Report("EXPAND_REENTRY", ok);
    return ok ? 0 : 1;
}

int ExpandRecover()
{
    void *before = CJ_CJThreadStackGuardGet();
    void *stackEnd = CJ_CJThreadStackAddrGet();
    uintptr_t reserved = CJ_CJThreadStackReversedGet();
    if (!Preconditions(before, stackEnd, reserved)) {
        Report("EXPAND_RECOVER", 0);
        return 2;
    }
    CJ_CJThreadStackGuardExpand();
    CJ_CJThreadStackGuardRecover();
    void *after = CJ_CJThreadStackGuardGet();
    // Target: the pair restores the guard, so a caught overflow leaves the next
    // threshold where it started and the boundary is reusable.
    int ok = after == before;
    std::fprintf(stderr, "SO_REENTRY_EXPAND_RECOVER_OK guard_before=%p guard_after=%p ok=%d\n",
        before, after, ok);
    Report("EXPAND_RECOVER", ok);
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// The real recovery entry: ExceptionManager::StackOverflow, entered the way the
// cangjie-runtime#1167 core entered it.
//
// The core's repeating prefix is ExceptionManager::StackOverflow ->
// ThrowImplicitException (ExceptionManager.cpp:358-370) -> ExecuteCangjieStub ->
// the raiser, and the raiser's own stack check brings the cycle back to
// StackOverflow before any clearer has run. This case builds that cycle for
// real: it registers a raiser through the product's own
// ExceptionManager::RegisterExceptionRaiser (ExceptionManager.h:86) and calls
// the product's ExceptionManager::StackOverflow (ExceptionManager.cpp:113)
// directly, so every turn runs the product's expand, the product's throw and
// the product's raiser dispatch. The raiser performs the one step the real
// consumer performs between turns — the marker clear of
// ExceptionWrapper::Reset/ClearInfo (ExceptionCApi.cpp:45,
// Mutator.cpp:194-197) — and re-enters the same product entry, so the recursion
// is the product's, not a loop that calls the guard API and returns.
//
// The turn count is bounded by the raiser itself: the cycle ends by decision,
// with a diagnosable end state, which is the invariant the issue states. A turn
// that could not end would end in a signal instead, and the run script reports
// that as a failure of this case.
// ---------------------------------------------------------------------------

// Turns of the real chain. The core recorded ~1100 before the wild dispatch;
// 64 separates "bounded" from "one step per turn" without depending on how deep
// a real overflow happened to go, and keeps the recursion inside the cjthread
// stack so the case measures the guard, not the c thread stack.
constexpr int kCycleTurns = 64;

MapleRuntime::Mutator *g_cycleMutator = nullptr;
int g_cycleDepth = 0;
int g_cycleTurnsDone = 0;
void *g_cycleGuardFirstTurn = nullptr;
uintptr_t g_cycleGuardLowest = UINTPTR_MAX;

void CycleRaiser(int type, void *threadData)
{
    (void)type;
    (void)threadData;
    g_cycleDepth++;
    if (g_cycleDepth > g_cycleTurnsDone) {
        g_cycleTurnsDone = g_cycleDepth;
    }
    // The product's own threshold after the turn's expand, read back the way the
    // managed runtime reads it. This is the value the target assertion consumes.
    uintptr_t guard = reinterpret_cast<uintptr_t>(CJ_CJThreadStackGuardGet());
    if (guard < g_cycleGuardLowest) {
        g_cycleGuardLowest = guard;
    }
    if (g_cycleDepth == 1) {
        g_cycleGuardFirstTurn = CJ_CJThreadStackGuardGet();
    }
    if (g_cycleDepth >= kCycleTurns) {
        // Bounded end: the cycle stops here, on purpose, with a readable state.
        return;
    }
    // What the consumer does before the next turn (ExceptionCApi.cpp:45): the
    // throwing marker is cleared. It does not run the guard recover — that is
    // the pair this issue is about, and the failing re-entry prefix in the core
    // never reached it.
    g_cycleMutator->GetExceptionWrapper().ClearInfo();
    // Re-enter the product's own overflow handler: the next turn is product code
    // from here to the raiser again.
    MapleRuntime::ExceptionManager::StackOverflow(0, reinterpret_cast<void *>(&CycleRaiser));
    g_cycleDepth--;
}

int ExpandCycle()
{
    void *before = CJ_CJThreadStackGuardGet();
    void *stackEnd = CJ_CJThreadStackAddrGet();
    uintptr_t reserved = CJ_CJThreadStackReversedGet();
    if (!Preconditions(before, stackEnd, reserved)) {
        Report("CYCLE", 0);
        return 2;
    }
    // The product's mutator, on this cjthread, so ExceptionManager::StackOverflow
    // has a real receiver with a real vtable and a real ExceptionWrapper — the
    // dispatch the core died in.
    g_cycleMutator = new MapleRuntime::Mutator();
    g_cycleMutator->InitTid();
    MapleRuntime::MutatorManager::Instance().BindMutator(*g_cycleMutator);
    MapleRuntime::Runtime::Current().GetExceptionManager().RegisterExceptionRaiser(
        reinterpret_cast<void *>(&CycleRaiser));
    std::fprintf(stderr, "SO_REENTRY_CYCLE_ARM mutator=%p raiser=registered turns=%d\n",
        static_cast<void *>(g_cycleMutator), kCycleTurns);
    g_cycleGuardLowest = reinterpret_cast<uintptr_t>(before);
    CycleRaiser(0, nullptr);
    void *after = CJ_CJThreadStackGuardGet();
    intptr_t walked = static_cast<intptr_t>(g_cycleGuardLowest) -
        reinterpret_cast<intptr_t>(after);
    std::fprintf(stderr,
        "SO_REENTRY_CYCLE_TURNS turns=%d deepest=%d guard_first_turn=%p guard_after=%p walked=%zd\n",
        kCycleTurns, g_cycleTurnsDone, g_cycleGuardFirstTurn, after, static_cast<ssize_t>(walked));
    // Target 1 (real recursion, product frames): every planned turn really ran
    // through the product's overflow handler. Proved by the depth the product's
    // own raiser reached, not by a call count in this file.
    bool deep = g_cycleTurnsDone == kCycleTurns;
    // Target 2 (bounded): the guard the product installed on the first turn is
    // the guard it still has, and it still names an address inside the stack.
    bool stable = g_cycleGuardFirstTurn != nullptr && after == g_cycleGuardFirstTurn;
    bool inside = reinterpret_cast<uintptr_t>(after) >= reinterpret_cast<uintptr_t>(stackEnd);
    int ok = deep && stable && inside ? 1 : 0;
    std::fprintf(stderr,
        "SO_REENTRY_CYCLE_OK turns=%d deepest=%d stable=%d above_stack_end=%d walked=%zd ok=%d\n",
        kCycleTurns, g_cycleTurnsDone, stable ? 1 : 0, inside ? 1 : 0, static_cast<ssize_t>(walked), ok);
    Report("CYCLE", ok);
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s once|bounded|recover|reentry|cycle\n", argv[0]);
        Finish(2);
    }
    int caseId = std::strcmp(argv[1], "once") == 0 ? 1
        : std::strcmp(argv[1], "bounded") == 0 ? 2
        : std::strcmp(argv[1], "recover") == 0 ? 3
        : std::strcmp(argv[1], "reentry") == 0 ? 4
        : std::strcmp(argv[1], "cycle") == 0 ? 5 : 0;
    if (caseId == 0) {
        std::fprintf(stderr, "SO_REENTRY_UNKNOWN_CASE name=%s\n", argv[1]);
        Finish(2);
    }
    // The product's own scheduler entry: from here on the calling thread is a cjthread
    // with an allocated stack, which is what the guard below belongs to.
    (void)new PublishedRuntime();
    if (SetupScheduler() == nullptr) {
        std::fprintf(stderr, "SO_REENTRY_SETUP_FAIL: default scheduler not created\n");
        Finish(2);
    }
    std::fprintf(stderr, "SO_REENTRY_SETUP_OK scheduler_created=1\n");
    switch (caseId) {
        case 1:
            Finish(ExpandOnce());
        case 2:
            Finish(ExpandBounded());
        case 3:
            Finish(ExpandRecover());
        case 4:
            Finish(ExpandReentry());
        case 5:
            Finish(ExpandCycle());
        default:
            Finish(2);
    }
}
