// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zThread.hpp"

#include "Base/Log.h"

namespace MapleRuntime {
// zThread.cpp:27-36
void ZThread::run_service()
{
    run_thread();

    std::unique_lock<std::mutex> ml(_terminator_lock);

    // Wait for signal to terminate
    while (!should_terminate()) {
        _terminator_condition.wait(ml);
    }
}

// zThread.cpp:38-49
void ZThread::stop_service()
{
    {
        // Signal thread to terminate
        // The should_terminate() flag should be true, and this notifies waiters
        // to wake up.
        std::lock_guard<std::mutex> ml(_terminator_lock);
        DCHECK(should_terminate());
        _terminator_condition.notify_all();
    }

    terminate();
}
} // namespace MapleRuntime
