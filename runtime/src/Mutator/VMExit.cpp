// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "VMExit.h"
#include "Common/Mutex.h"
#include "Mutator.h"
#include "SafepointSynchronize.h"
#include "ThreadSMR.h"
#include "VMOperation.h"
#include <atomic>
#include <new>

namespace MapleRuntime {
namespace {
enum class BeforeExit { NotRun, Running, Done, Failed };
// A native thread ID or TLS address can be reused after the host owner exits.
// Retain the owner's identity object, as HotSpot retains _shutdown_thread.
struct NativeThreadIdentity {};
thread_local NativeThreadIdentity* currentShutdownIdentity = nullptr;
struct ExitState {
    Monitor monitor;
    std::atomic<BeforeExit> before{BeforeExit::NotRun};
    std::atomic<bool> vmExited{false};
    size_t nativeCount = 0;
    NativeThreadIdentity* owner = nullptr;
};
ExitState& state()
{
    static ExitState* const value = new ExitState;
    return *value;
}
}

bool VM_Exit::begin_before_exit(bool (*qualify)())
{
    MonitorLocker lock(state().monitor);
    while (state().before.load() == BeforeExit::Running) { lock.wait(); }
    if (state().before.load() != BeforeExit::NotRun) { return false; }
    if (qualify != nullptr && !qualify()) { return false; }
    currentShutdownIdentity = new (std::nothrow) NativeThreadIdentity;
    CHECK_DETAIL(currentShutdownIdentity != nullptr, "shutdown identity allocation failed");
    state().owner = currentShutdownIdentity;
    state().before.store(BeforeExit::Running, std::memory_order_release);
    return true;
}

void VM_Exit::complete_before_exit(bool success)
{
    MonitorLocker lock(state().monitor);
    CHECK_DETAIL(is_shutdown_thread(), "only shutdown owner completes before_exit");
    CHECK_DETAIL(!success || exited(), "logical termination must precede success");
    state().before.store(success ? BeforeExit::Done : BeforeExit::Failed, std::memory_order_release);
    state().monitor.notify_all();
}

bool VM_Exit::closing() { return state().before.load(std::memory_order_acquire) != BeforeExit::NotRun; }
bool VM_Exit::exited() { return state().vmExited.load(std::memory_order_acquire); }
bool VM_Exit::is_shutdown_thread()
{
    return closing() && currentShutdownIdentity != nullptr && state().owner == currentShutdownIdentity;
}

bool VM_Exit::is_native(const Mutator& mutator)
{
    return mutator.InSaferegion() &&
        const_cast<Mutator&>(mutator).GetUnwindContext().GetUnwindContextStatus() == UnwindContextStatus::RISKY;
}

size_t VM_Exit::set_vm_exited()
{
    CHECK_DETAIL(is_shutdown_thread(), "terminal publication requires shutdown ownership");
    CHECK_DETAIL(SafepointSynchronize::is_at_safepoint() && VMThread::is_terminated(),
                 "terminal publication requires the permanent final safepoint");
    size_t nativeCount = 0;
    ThreadsListHandle threads;
    for (size_t i = 0; i < threads.length(); ++i) {
        Mutator* mutator = threads.thread_at(i);
        if (is_native(*mutator)) {
            mutator->SetTerminatedByVM();
            ++nativeCount;
        }
    }
    state().nativeCount = nativeCount;
    state().vmExited.store(true, std::memory_order_release);
    return nativeCount;
}

size_t VM_Exit::native_threads_at_exit()
{
    CHECK_DETAIL(exited(), "native inventory is only final after terminal publication");
    return state().nativeCount;
}

void VM_Exit::wait_if_vm_exited()
{
    if (exited() && !is_shutdown_thread()) {
        // vmOperations.cpp:584-595: the final Threads_lock is never released.
        SafepointSynchronize::state().mutatorManagementRWLock.LockRead();
        CHECK_DETAIL(false, "returned through permanent VM exit barrier");
    }
}

void VM_Exit::wait_for_native_entry()
{
    if (closing()) {
        MonitorLocker lock(state().monitor);
        while (state().before.load() == BeforeExit::Running) { lock.wait(); }
        wait_if_vm_exited();
        CHECK_DETAIL(false, "native entry after unsuccessful VM shutdown");
    }
}
} // namespace MapleRuntime
