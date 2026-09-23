// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zJNICritical.hpp"
#include "Common/ScopedObjectAccess.h"

namespace MapleRuntime {

std::atomic<int64_t> ZJNICritical::count{ 0 };
std::mutex ZJNICritical::lock;
std::condition_variable ZJNICritical::attention;
std::once_flag ZJNICritical::once;

void ZJNICritical::initialize()
{
    std::call_once(once, [] {
        count.store(0, std::memory_order_relaxed);
    });
}

void ZJNICritical::block()
{
    initialize();
    for (;;) {
        const int64_t n = count.load(std::memory_order_acquire);
        if (n < 0) {
            std::unique_lock<std::mutex> guard(lock);
            while (count.load(std::memory_order_acquire) < 0) {
                attention.wait(guard);
            }
            continue;
        }
        int64_t expected = n;
        if (!count.compare_exchange_strong(expected, -(n + 1), std::memory_order_acq_rel)) {
            continue;
        }
        if (n != 0) {
            std::unique_lock<std::mutex> guard(lock);
            while (count.load(std::memory_order_acquire) != -1) {
                attention.wait(guard);
            }
        }
        return;
    }
}

void ZJNICritical::unblock()
{
    initialize();
    std::lock_guard<std::mutex> guard(lock);
    count.store(0, std::memory_order_release);
    attention.notify_all();
}

void ZJNICritical::enter_inner()
{
    for (;;) {
        const int64_t n = count.load(std::memory_order_acquire);
        if (n < 0) {
            // ZGC zJNICritical.cpp:108-116: publish a blockable thread state
            // before taking the condition lock, so a concurrent handshake can
            // complete while this mutator waits for JNI critical to unblock.
            ScopedEnterSaferegion enterSaferegion(true);
            std::unique_lock<std::mutex> guard(lock);
            while (count.load(std::memory_order_acquire) < 0) {
                attention.wait(guard);
            }
            continue;
        }
        int64_t expected = n;
        if (!count.compare_exchange_strong(expected, n + 1, std::memory_order_acq_rel)) {
            continue;
        }
        return;
    }
}

void ZJNICritical::enter()
{
    initialize();
    enter_inner();
}

void ZJNICritical::exit_inner()
{
    for (;;) {
        const int64_t n = count.load(std::memory_order_acquire);
        if (n > 0) {
            int64_t expected = n;
            if (!count.compare_exchange_strong(expected, n - 1, std::memory_order_acq_rel)) {
                continue;
            }
            return;
        }
        int64_t expected = n;
        if (!count.compare_exchange_strong(expected, n + 1, std::memory_order_acq_rel)) {
            continue;
        }
        if (n == -2) {
            std::lock_guard<std::mutex> guard(lock);
            attention.notify_all();
        }
        return;
    }
}

void ZJNICritical::exit()
{
    initialize();
    exit_inner();
}

} // namespace MapleRuntime
