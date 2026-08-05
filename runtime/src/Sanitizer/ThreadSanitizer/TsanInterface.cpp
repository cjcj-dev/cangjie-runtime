// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include <atomic>

#include "TsanInterface.h"

#include "Base/Log.h"
#include "CJThreadRecorder.h"
#include "Sanitizer/SanitizerCompilerCalls.h"
#include "Sanitizer/SanitizerMacros.h"
#include "Sanitizer/SanitizerSymbols.h"
#include "schedule.h"

namespace MapleRuntime {
namespace Sanitizer {
using namespace std;
using RaceStateHandle = void*;
using RaceProcHandle = void*;

static void* g_tsanRuntimeSync = nullptr;
static std::atomic<bool> g_initialized{false};
static CJThreadRecorder<RaceProcHandle> g_procState{};

// TLS race state for native (non-cjthread) threads such as GC pool workers.
// Cangjie-TSan resolves thr via CJ_MCC_TsanGetThreadState; without a cjthread
// slot that returns null and all memory/atomic hooks become no-ops.
static thread_local RaceStateHandle g_nativeRaceState = nullptr;
static thread_local RaceProcHandle g_nativeRaceProc = nullptr;

void TsanInitialize()
{
    void* cjthread = CJThreadGetHandle();
    // __tsan_init returns a state that has to live in the calling cjthread's own
    // sanitizer slot. A null current cjthread, or one with no stack of its own
    // (foreign/exclusive), has no slot to put it in: those run with a null race state
    // and their tracking hooks are no-ops by design, while managed owned-stack
    // cjthreads stay tracked.
    if (cjthread == nullptr || CJThreadStackBaseAddrGet() == nullptr) {
        return;
    }
    // Idempotent per slot, not per process. Every scheduler's thread0 still gets its
    // own state the first time it initializes — that is the upstream behaviour, and a
    // process-wide once would leave every scheduler after the first untracked. What
    // repetition must not do is overwrite a state that is already there: that was the
    // defect, where a sub-scheduler created from a tracked cjthread replaced its
    // parent's state and leaked the old one. The slot belongs to one cjthread, which
    // cannot race itself here.
    if (CJThreadGetSanitizerContext(cjthread) != nullptr) {
        return;
    }
    CJThreadSetSanitizerContext(cjthread, REAL(__tsan_init)());
    // One-way gate for the getters below: once any cjthread is tracked, TSAN is live.
    // Concurrent stores of the same value are harmless.
    g_initialized.store(true, std::memory_order_release);
}

void TsanAttachNativeThread()
{
    if (g_nativeRaceState != nullptr) {
        return;
    }
    // Root ThreadState for this OS thread (same allocator path as cjthread init).
    g_nativeRaceState = REAL(__tsan_init)();
    g_nativeRaceProc = REAL(__tsan_proc_create)();
    g_initialized.store(true, std::memory_order_release);
}

void TsanDetachNativeThread()
{
    if (g_nativeRaceState != nullptr) {
        REAL(__tsan_state_delete)(g_nativeRaceState);
        g_nativeRaceState = nullptr;
    }
    if (g_nativeRaceProc != nullptr) {
        REAL(__tsan_proc_destroy)(g_nativeRaceProc);
        g_nativeRaceProc = nullptr;
    }
}

void TsanFinalize()
{
    REAL(__tsan_fini)();
}

void OnHeapAllocated(void* addr, size_t size)
{
    REAL(__tsan_init_shadow)(addr, size);
}

void OnHeapDeallocated(void*, size_t) {}

void TsanFree(void* addr, size_t size)
{
    REAL(__tsan_free)(__builtin_return_address(0), addr, size);
}

static inline RaceStateHandle CJThreadGetCurRaceState()
{
    void* cjthread = CJThreadGetHandle();
    if (cjthread != nullptr) {
        RaceStateHandle rs = CJThreadGetSanitizerContext(cjthread);
        if (rs != nullptr) {
            return rs;
        }
    }
    return g_nativeRaceState;
}

void TsanAcquire()
{
    REAL(__tsan_acquire)(CJThreadGetCurRaceState(), &g_tsanRuntimeSync);
}

void TsanRelease(ReleaseType rm)
{
    TsanRelease(&g_tsanRuntimeSync, rm);
}

void TsanAcquire(const void* addr)
{
    REAL(__tsan_acquire)(CJThreadGetCurRaceState(), addr);
}

void TsanRelease(const void* addr, ReleaseType rm)
{
    RaceStateHandle rs = CJThreadGetCurRaceState();
    switch (rm) {
        case ReleaseType::K_RELEASE:
            REAL(__tsan_release)(rs, addr);
            break;
        case ReleaseType::K_RELEASE_MERGE:
            REAL(__tsan_release_merge)(rs, addr);
            break;
        case ReleaseType::K_RELEASE_ACQUIRE:
            REAL(__tsan_release_acquire)(rs, addr);
            break;
    }
}

void TsanFixShadow(const void* from, const void* to, size_t size)
{
    REAL(__tsan_fix_shadow)(from, to, size);
}

void TsanAllocObject(const void* addr, size_t size)
{
    void* pc = __builtin_return_address(0);
    REAL(__tsan_alloc)(pc, addr, size);
}

void TsanFuncEntry(const void* pc)
{
    REAL(__tsan_func_entry)(pc);
}

void TsanFuncExit()
{
    REAL(__tsan_func_exit)();
}

void TsanFuncRestoreContext(const void* pc)
{
    REAL(__tsan_func_restore_context)(pc);
}

void TsanWriteMemory(const void* addr, size_t size)
{
    REAL(__tsan_write)(__builtin_return_address(0), addr, size);
}

void TsanReadMemory(const void* addr, size_t size)
{
    REAL(__tsan_read)(__builtin_return_address(0), addr, size);
}

void TsanWriteMemoryRange(const void* addr, size_t size)
{
    REAL(__tsan_write_range)(__builtin_return_address(0), addr, size);
}

void TsanReadMemoryRange(const void* addr, size_t size)
{
    REAL(__tsan_read_range)(__builtin_return_address(0), addr, size);
}

void TsanCleanShadow(const void* addr, size_t size)
{
    REAL(__tsan_clean_shadow)(addr, size);
}

void TsanNewRaceState(void* cjthread, void* parent, const void* pc)
{
    if (parent == nullptr) {
        return;
    }

    RaceStateHandle pRaceState = CJThreadGetSanitizerContext(parent);
    if (pRaceState) {
        CJThreadSetSanitizerContext(cjthread, REAL(__tsan_state_create)(pRaceState, pc));
    }
}

void TsanDeleteRaceState(void* thread)
{
    REAL(__tsan_state_delete)(CJThreadGetSanitizerContext(thread));
    CJThreadSetSanitizerContext(thread, nullptr);
}

void TsanNewRaceProc(void* processor)
{
    g_procState.CreateThread(processor, REAL(__tsan_proc_create)());
}

void TsanDeleteRaceProc(void* processor)
{
    REAL(__tsan_proc_destroy)(g_procState.DeleteThread(processor));
}

extern "C" {
MRT_EXPORT void* CJ_MCC_TsanGetRaceProc(void)
{
    if (!g_initialized.load(std::memory_order_acquire)) {
        return nullptr;
    }
    if (g_nativeRaceProc != nullptr) {
        return g_nativeRaceProc;
    }
    void* processor = ProcessorGetHandle();
    return g_procState.GetDataFromThread(processor);
}

MRT_EXPORT void* CJ_MCC_TsanGetThreadState(void)
{
    if (!g_initialized.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return CJThreadGetCurRaceState();
}

MRT_EXPORT void CJ_MCC_TsanWriteMemory(const void* addr, size_t size)
{
    REAL(__tsan_write)(__builtin_return_address(0), addr, size);
}

MRT_EXPORT void CJ_MCC_TsanReadMemory(const void* addr, size_t size)
{
    REAL(__tsan_read)(__builtin_return_address(0), addr, size);
}

MRT_EXPORT void CJ_MCC_TsanWriteMemoryRange(const void* addr, size_t size)
{
    REAL(__tsan_write_range)(__builtin_return_address(0), addr, size);
}

MRT_EXPORT void CJ_MCC_TsanReadMemoryRange(const void* addr, size_t size)
{
    REAL(__tsan_read_range)(__builtin_return_address(0), addr, size);
}
}
} // namespace Sanitizer
} // namespace MapleRuntime
