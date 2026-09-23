// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#ifndef MUTATOR_LOCK_POSTCHECK
#error "MUTATOR_LOCK_POSTCHECK must describe the MutatorManager.h revision under test"
#endif

namespace MapleRuntime {
namespace {
constexpr int WRITE_LOCKED = -1;
constexpr uint32_t STRESS_READERS = 32;
constexpr uint32_t STRESS_WRITES = 200;
constexpr uint64_t STRESS_PREWARM_READS = 10000;
constexpr auto PROBE_DEADLINE = std::chrono::seconds(5);

class RwLockModel {
public:
    void LockRead()
    {
        int count = lockCount.load(std::memory_order_acquire);
        do {
            while (count == WRITE_LOCKED) {
                std::this_thread::yield();
                count = lockCount.load(std::memory_order_acquire);
            }
        } while (!lockCount.compare_exchange_weak(count, count + 1, std::memory_order_release));
    }

    bool TryLockRead()
    {
        int count = lockCount.load(std::memory_order_acquire);
        do {
            if (count == WRITE_LOCKED) {
                return false;
            }
        } while (!lockCount.compare_exchange_weak(count, count + 1, std::memory_order_release));
        return true;
    }

    bool TryLockWrite()
    {
        int count = 0;
        return lockCount.compare_exchange_weak(count, WRITE_LOCKED, std::memory_order_release);
    }

    void UnlockRead() { (void)lockCount.fetch_sub(1); }

    void UnlockWrite() { lockCount.store(0, std::memory_order_release); }

private:
    std::atomic<int> lockCount { 0 };
};

class MutatorManagementLockModel {
public:
    bool TryAcquireMutatorManagementRLock()
    {
        if (mgmtWritersWaiting.load(std::memory_order_acquire) > 0) {
            return false;
        }
        if (!mutatorManagementRWLock.TryLockRead()) {
            return false;
        }
#if MUTATOR_LOCK_POSTCHECK
        if (mgmtWritersWaiting.load(std::memory_order_acquire) > 0) {
            mutatorManagementRWLock.UnlockRead();
            return false;
        }
#endif
        return true;
    }

    void MutatorManagementRUnlock() { mutatorManagementRWLock.UnlockRead(); }

    void AnnounceMgmtWriterPending() { mgmtWritersWaiting.fetch_add(1, std::memory_order_acq_rel); }

    void WithdrawMgmtWriterPending() { mgmtWritersWaiting.fetch_sub(1, std::memory_order_acq_rel); }

    bool TryAcquireMutatorManagementWLock() { return mutatorManagementRWLock.TryLockWrite(); }

    void MutatorManagementWUnlock() { mutatorManagementRWLock.UnlockWrite(); }

    std::atomic<uint32_t>& WaitingWriters() { return mgmtWritersWaiting; }

    RwLockModel& Lock() { return mutatorManagementRWLock; }

private:
    RwLockModel mutatorManagementRWLock;
    std::atomic<uint32_t> mgmtWritersWaiting { 0 };
};

bool WaitUntil(const std::atomic<bool>& flag, std::chrono::steady_clock::time_point deadline)
{
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

bool RunAdmissionInterleaving()
{
    MutatorManagementLockModel model;
    std::atomic<bool> readerChecked { false };
    std::atomic<bool> writerAnnounced { false };
    std::atomic<bool> readerDecided { false };
    std::atomic<bool> writerAttempted { false };
    std::atomic<bool> readerAdmitted { false };
    std::atomic<bool> writerAcquired { false };
    std::atomic<bool> timedOut { false };
    auto deadline = std::chrono::steady_clock::now() + PROBE_DEADLINE;

    std::thread reader([&]() {
        if (model.WaitingWriters().load(std::memory_order_acquire) != 0) {
            timedOut.store(true, std::memory_order_release);
            return;
        }
        readerChecked.store(true, std::memory_order_release);
        if (!WaitUntil(writerAnnounced, deadline)) {
            timedOut.store(true, std::memory_order_release);
            return;
        }

        bool admitted = model.Lock().TryLockRead();
#if MUTATOR_LOCK_POSTCHECK
        if (admitted && model.WaitingWriters().load(std::memory_order_acquire) > 0) {
            model.Lock().UnlockRead();
            admitted = false;
        }
#endif
        readerAdmitted.store(admitted, std::memory_order_release);
        readerDecided.store(true, std::memory_order_release);
        if (admitted) {
            if (!WaitUntil(writerAttempted, deadline)) {
                timedOut.store(true, std::memory_order_release);
            }
            model.MutatorManagementRUnlock();
        }
    });

    std::thread writer([&]() {
        if (!WaitUntil(readerChecked, deadline)) {
            timedOut.store(true, std::memory_order_release);
            return;
        }
        model.AnnounceMgmtWriterPending();
        writerAnnounced.store(true, std::memory_order_release);
        if (!WaitUntil(readerDecided, deadline)) {
            timedOut.store(true, std::memory_order_release);
            model.WithdrawMgmtWriterPending();
            return;
        }
        bool acquired = model.TryAcquireMutatorManagementWLock();
        writerAcquired.store(acquired, std::memory_order_release);
        writerAttempted.store(true, std::memory_order_release);
        if (acquired) {
            model.MutatorManagementWUnlock();
        }
        model.WithdrawMgmtWriterPending();
    });

    reader.join();
    writer.join();
    bool admitted = readerAdmitted.load(std::memory_order_acquire);
    bool acquired = writerAcquired.load(std::memory_order_acquire);
    bool expected = MUTATOR_LOCK_POSTCHECK ? (!admitted && acquired) : (admitted && !acquired);
    bool passed = !timedOut.load(std::memory_order_acquire) && expected;
    std::cout << "MUTATOR_LOCK_INTERLEAVING postcheck=" << MUTATOR_LOCK_POSTCHECK
              << " reader_admitted_after_announcement=" << admitted
              << " writer_first_try=" << acquired
              << " wait_cycle=" << (admitted && !acquired ? "EXPOSED" : "CLOSED")
              << " result=" << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

bool RunFourCoreStress()
{
    MutatorManagementLockModel model;
    std::atomic<bool> start { false };
    std::atomic<bool> stop { false };
    std::atomic<uint32_t> ready { 0 };
    std::atomic<uint64_t> reads { 0 };
    std::atomic<uint64_t> deferredReads { 0 };
    std::vector<std::thread> readers;
    readers.reserve(STRESS_READERS);
    for (uint32_t idx = 0; idx < STRESS_READERS; ++idx) {
        readers.emplace_back([&]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!stop.load(std::memory_order_acquire)) {
                if (model.TryAcquireMutatorManagementRLock()) {
                    reads.fetch_add(1, std::memory_order_relaxed);
                    model.MutatorManagementRUnlock();
                } else {
                    deferredReads.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                }
            }
        });
    }

    auto prewarmDeadline = std::chrono::steady_clock::now() + PROBE_DEADLINE;
    while (ready.load(std::memory_order_acquire) != STRESS_READERS &&
        std::chrono::steady_clock::now() < prewarmDeadline) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    while (reads.load(std::memory_order_acquire) < STRESS_PREWARM_READS &&
        std::chrono::steady_clock::now() < prewarmDeadline) {
        std::this_thread::yield();
    }
    uint64_t maxWriterWaitUs = 0;
    bool timedOut = ready.load(std::memory_order_acquire) != STRESS_READERS ||
        reads.load(std::memory_order_acquire) < STRESS_PREWARM_READS;
    for (uint32_t round = 0; round < STRESS_WRITES; ++round) {
        model.AnnounceMgmtWriterPending();
        auto begin = std::chrono::steady_clock::now();
        auto deadline = begin + PROBE_DEADLINE;
        while (!model.TryAcquireMutatorManagementWLock()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                timedOut = true;
                break;
            }
            std::this_thread::yield();
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin).count();
        if (static_cast<uint64_t>(elapsed) > maxWriterWaitUs) {
            maxWriterWaitUs = static_cast<uint64_t>(elapsed);
        }
        if (!timedOut) {
            model.MutatorManagementWUnlock();
        }
        model.WithdrawMgmtWriterPending();
        if (timedOut) {
            break;
        }
    }
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }

    std::cout << "MUTATOR_LOCK_4CORE_STRESS postcheck=" << MUTATOR_LOCK_POSTCHECK
              << " readers=" << STRESS_READERS
              << " writes=" << STRESS_WRITES
              << " reads=" << reads.load(std::memory_order_relaxed)
              << " deferred_reads=" << deferredReads.load(std::memory_order_relaxed)
              << " max_writer_wait_us=" << maxWriterWaitUs
              << " timed_out=" << timedOut
              << " result=" << (!timedOut ? "PASS" : "FAIL") << '\n';
    return !timedOut;
}
} // namespace
} // namespace MapleRuntime

int main()
{
    bool interleavingPassed = MapleRuntime::RunAdmissionInterleaving();
    bool stressPassed = MapleRuntime::RunFourCoreStress();
    return interleavingPassed && stressPassed ? EXIT_SUCCESS : EXIT_FAILURE;
}
