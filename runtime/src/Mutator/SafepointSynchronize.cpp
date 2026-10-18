// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "SafepointSynchronize.h"
#include "Base/Panic.h"
#include "Base/SysCall.h"
#include <new>

namespace MapleRuntime {
SafepointState& SafepointSynchronize::state()
{
    // Intentionally not destroyed during exit(): native carriers may still
    // return, or run their TLS destructors, after Runtime has been deleted.
    static SafepointState* const value = new SafepointState;
    return *value;
}

void SafepointSynchronize::initialize()
{
#if !defined(_WIN64)
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        state().safepointPageManager = new (std::nothrow) SafepointPageManager;
        CHECK_DETAIL(state().safepointPageManager != nullptr, "new safepointPageManager failed");
        state().safepointPageManager->Init();
    });
#endif
}

void SafepointSynchronize::block()
{
    auto& sync = state();
    // Spurious wakes never release a final pause. The normal producer clears
    // the same word before waking waiters; the final producer never clears it.
    for (;;) {
        const uint32_t count = sync.suspensionMutatorCount.load(std::memory_order_acquire);
        if (count == 0) { return; }
#if defined(_WIN64) || defined(__APPLE__)
        std::unique_lock<std::mutex> lock(sync.mutatorSuspensionMtx);
        sync.mutatorSuspensionCV.wait(lock, [&sync] {
            return sync.suspensionMutatorCount.load(std::memory_order_acquire) == 0;
        });
#else
        (void)Futex(reinterpret_cast<int*>(&sync.suspensionMutatorCount), FUTEX_WAIT,
                    static_cast<int>(count));
#endif
    }
}
} // namespace MapleRuntime
