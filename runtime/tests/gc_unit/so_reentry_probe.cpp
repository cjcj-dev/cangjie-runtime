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
    PublishedRuntime() { runtime = this; }
    ~PublishedRuntime() override { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return {}; }
    void SetGCThreshold(uint64_t) override {}
};

// The scheduler calls the registered TLS hook when it hands a cjthread the runtime
// thread-local block (schedule.h:1265 ScheduleGetTlsHookRegister, registered by
// CJThreadModel::Init at CJThreadModel.cpp:203 with this very function).
extern "C" uintptr_t *MRT_GetThreadLocalData(void);

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
    if (CJ_ScheduleGetTlsHookRegister(MRT_GetThreadLocalData) != 0) {
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

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s once|bounded|recover\n", argv[0]);
        Finish(2);
    }
    int caseId = std::strcmp(argv[1], "once") == 0 ? 1
        : std::strcmp(argv[1], "bounded") == 0 ? 2
        : std::strcmp(argv[1], "recover") == 0 ? 3 : 0;
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
        default:
            Finish(2);
    }
}
