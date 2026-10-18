// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/shared/gcVMOperations.hpp"
#include "CangjieRuntime.h"
#include "Common/Mutex.h"
#include "Heap/z/concurrentGCThread.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zHeap.hpp"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
bool VM_Heap_Sync_Operation::doit_prologue()
{
    HeapMonitor().lock();
    return true;
}

void VM_Heap_Sync_Operation::doit_epilogue() { HeapMonitor().unlock(); }

VM_GC_Operation::VM_GC_Operation(uint32_t gcCountBefore, bool full,
                               uint32_t fullGCCountBefore, GCReason cause)
    : gcCause(cause), gcCountBefore(gcCountBefore), fullGCCountBefore(fullGCCountBefore), full(full) {}

bool VM_GC_Operation::skip_operation() const
{
    bool skip = gcCountBefore != ZCollectedHeap::heap()->total_collections();
    if (full && skip) {
        skip = fullGCCountBefore != ZCollectedHeap::heap()->total_full_collections();
    }
    return skip;
}

// gcVMOperations.cpp:95-109. A real registered managed caller waits safely;
// a real ConcurrentGCThread refuses submission and drains before final pause.
static void block_if_managed_thread()
{
    if (ConcurrentGCThread::current() != nullptr) { return; }
    Mutator* const mutator = Mutator::GetMutator();
    if (mutator != nullptr && mutator->GetCjthreadPtr() != nullptr && !mutator->IsGCDetached()) {
        for (;;) { HeapMonitor().wait(); }
    }
    CHECK_DETAIL(false,
                 "heap service shutdown requires a registered managed or ConcurrentGC caller");
}

bool VM_GC_Operation::doit_prologue()
{
    CHECK_DETAIL(gcCause != GC_REASON_INVALID, "illegal GC cause");
    CHECK_DETAIL(CangjieRuntime::IsInitialized(), "GC service before VM initialization completed");
    VM_Heap_Sync_Operation::doit_prologue();
    shuttingDown = ZCollectedHeap::heap()->is_shutting_down();
    if (shuttingDown) { block_if_managed_thread(); }
    prologueSucceeded = !(skip_operation() || shuttingDown);
    if (!prologueSucceeded) { HeapMonitor().unlock(); }
    return prologueSucceeded;
}

void VM_GC_Operation::doit_epilogue()
{
    if (Heap::GetHeap().GetFinalizerProcessor().HasPendingList()) {
        HeapMonitor().notify_all();
    }
    VM_Heap_Sync_Operation::doit_epilogue();
}
} // namespace MapleRuntime
