// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zJNICritical.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zLock.inline.hpp"
#include "Base/Log.h"
#include "Common/ScopedObjectAccess.h"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
static const ZStatCriticalPhase ZCriticalPhaseJNICriticalStall("JNI Critical Stall");

std::atomic<int64_t> ZJNICritical::count{ 0 };
ZConditionLock* ZJNICritical::lock = nullptr;

void ZJNICritical::initialize()
{
    CHECK_DETAIL(count.load(std::memory_order_relaxed) == 0, "Invalid initial count");
    lock = new ZConditionLock();
}

void ZJNICritical::block()
{
    for (;;) {
        const int64_t n = count.load(std::memory_order_acquire);
        if (n < 0) {
            ZLocker<ZConditionLock> guard(lock);
            while (count.load(std::memory_order_acquire) < 0) {
                lock->wait();
            }
            continue;
        }
        int64_t expected = n;
        if (!count.compare_exchange_strong(expected, -(n + 1), std::memory_order_acq_rel)) {
            continue;
        }
        if (n != 0) {
            ZLocker<ZConditionLock> guard(lock);
            while (count.load(std::memory_order_acquire) != -1) {
                lock->wait();
            }
        }
        return;
    }
}

void ZJNICritical::unblock()
{
    const int64_t n = count.load(std::memory_order_acquire);
    CHECK_DETAIL(n == -1, "Invalid count");
    ZLocker<ZConditionLock> guard(lock);
    count.store(0, std::memory_order_release);
    lock->notify_all();
}

void ZJNICritical::enter_inner()
{
    for (;;) {
        const int64_t n = count.load(std::memory_order_acquire);
        if (n < 0) {
            ZStatTimer timer(ZCriticalPhaseJNICriticalStall);
            // ZGC zJNICritical.cpp:108-116: publish a blockable thread state
            // before taking the condition lock, so a concurrent handshake can
            // complete while this mutator waits for JNI critical to unblock.
            ScopedEnterSaferegion enterSaferegion(true);
            ZLocker<ZConditionLock> guard(lock);
            while (count.load(std::memory_order_acquire) < 0) {
                lock->wait();
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
    Mutator* thread = Mutator::GetMutator();
    // ZGC zJNICritical.cpp:132-140: only the outermost region enters globally.
    if (!thread->InCritical()) {
        enter_inner();
    }
    thread->EnterCritical();
}

void ZJNICritical::exit_inner()
{
    for (;;) {
        const int64_t n = count.load(std::memory_order_acquire);
        CHECK_DETAIL(n != 0, "Invalid count");
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
            ZLocker<ZConditionLock> guard(lock);
            lock->notify_all();
        }
        return;
    }
}

void ZJNICritical::exit()
{
    Mutator* thread = Mutator::GetMutator();
    // ZGC zJNICritical.cpp:177-184: release globally after the last nested exit.
    thread->ExitCritical();
    if (!thread->InCritical()) {
        exit_inner();
    }
}

} // namespace MapleRuntime
