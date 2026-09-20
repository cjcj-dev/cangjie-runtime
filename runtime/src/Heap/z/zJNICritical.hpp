// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_JNI_CRITICAL_HPP
#define MRT_Z_JNI_CRITICAL_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace MapleRuntime {

// ZJNICritical (zJNICritical.hpp:33-50; zJNICritical.cpp:51-181): process-wide
// count + condition lock. count>=0 enter allowed; -1 blocked; < -1 block in progress.
class ZJNICritical {
public:
    static void initialize();
    static void block();
    static void unblock();
    static void enter();
    static void exit();
    static int64_t count_snapshot() { return count.load(std::memory_order_acquire); }

private:
    static void enter_inner();
    static void exit_inner();

    static std::atomic<int64_t> count;
    static std::mutex lock;
    static std::condition_variable attention;
    static std::once_flag once;
};

} // namespace MapleRuntime

#endif
