// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zJNICritical.hpp"

#include <chrono>
#include "Heap/z/zStat.hpp"
#include "Heap/z/zLock.inline.hpp"
#include "Base/Log.h"
#include "Common/ScopedObjectAccess.h"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
static const ZStatCriticalPhase ZCriticalPhaseJNICriticalStall("JNI Critical Stall");

std::atomic<int64_t> ZJNICritical::count{ 0 };
ZConditionLock* ZJNICritical::lock = nullptr;
int64_t ZJNICritical::boundedBlockers = 0;
std::atomic<bool> ZJNICritical::stuck{ false };

void ZJNICritical::initialize()
{
    CHECK_DETAIL(count.load(std::memory_order_relaxed) == 0, "Invalid initial count");
    lock = new ZConditionLock();
}

// A raw-data holder may legally block in a foreign call until another mutator
// acts (std.core acquireArrayRawData only forbids the holder itself from
// allocating). An unbounded block (JDK zJNICritical.cpp block()) would then wait
// forever. The collector instead waits a bounded time; if holders remain it
// restores the count and proceeds without excluding them, and its caller must
// then not move any object for that operation (ZRawDataRelocationGate). While
// such a holder stays inside (`stuck`), later calls report the timeout at once.
// A drained block may be shared by more than one collector party; the last
// unblock_bounded() ends it.
bool ZJNICritical::block_bounded(uint64_t millis)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(millis);
    auto remaining = [&deadline]() -> uint64_t {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return 0;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        return left > 0 ? static_cast<uint64_t>(left) : 1;
    };
    ZLocker<ZConditionLock> guard(lock);
    for (;;) {
        const int64_t n = count.load(std::memory_order_acquire);
        if (n > 0 && stuck.load(std::memory_order_acquire)) {
            // A holder that outlived an earlier bound is still inside: report the
            // timeout now rather than keep every other acquirer out for a full bound.
            return false;
        }
        if (n < 0) {
            if (n == -1 && boundedBlockers > 0) {
                // Another collector party owns a drained block: no holder can be
                // inside and none can enter until the last party unblocks.
                boundedBlockers++;
                return true;
            }
            // An exclusive block(), or another party still draining.
            const uint64_t left = remaining();
            if (left == 0) {
                return false;
            }
            (void)lock->wait(left);
            continue;
        }
        int64_t expected = n;
        if (!count.compare_exchange_strong(expected, -(n + 1), std::memory_order_acq_rel)) {
            continue;
        }
        boundedBlockers = 1;
        while (count.load(std::memory_order_acquire) != -1) {
            const uint64_t left = remaining();
            if (left == 0) {
                // Holders are still inside (e.g. blocked in a foreign call).
                // Restore -(m + 1) -> m; exit_inner may race, hence the CAS loop.
                // Mark stuck before the restore so that the exit which later drains
                // the count to 0 is the one that clears it.
                stuck.store(true, std::memory_order_release);
                for (;;) {
                    int64_t c = count.load(std::memory_order_acquire);
                    if (c >= 0) {
                        break;
                    }
                    if (count.compare_exchange_strong(c, -(c + 1), std::memory_order_acq_rel)) {
                        if (c == -1) {
                            // The last holder left during the timeout path.
                            stuck.store(false, std::memory_order_release);
                        }
                        break;
                    }
                }
                boundedBlockers = 0;
                lock->notify_all();
                return false;
            }
            (void)lock->wait(left);
        }
        return true;
    }
}

void ZJNICritical::unblock_bounded()
{
    ZLocker<ZConditionLock> guard(lock);
    CHECK_DETAIL(boundedBlockers > 0, "Invalid bounded blockers");
    CHECK_DETAIL(count.load(std::memory_order_acquire) == -1, "Invalid count");
    if (--boundedBlockers == 0) {
        count.store(0, std::memory_order_release);
        lock->notify_all();
    }
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
            if (n == 1) {
                stuck.store(false, std::memory_order_release);
            }
            return;
        }
        int64_t expected = n;
        if (!count.compare_exchange_strong(expected, n + 1, std::memory_order_acq_rel)) {
            continue;
        }
        if (n == -2) {
            stuck.store(false, std::memory_order_release);
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
