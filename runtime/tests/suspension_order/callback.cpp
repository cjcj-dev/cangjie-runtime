// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"

namespace {
std::atomic<unsigned> reached{0};
// Bounded rendezvous, not a probability or allocation-pressure test.
template<class Predicate> bool Wait(Predicate predicate)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) { return false; }
        std::this_thread::yield();
    }
    return true;
}
}
extern "C" int64_t R06Reached()
{
    reached.fetch_add(1, std::memory_order_release);
    return 0;
}
extern "C" int64_t R06Invoke(int64_t (*callback)(), bool stopped)
{
    using namespace MapleRuntime;
    auto* target = ThreadLocal::GetMutator();
    if (target == nullptr || !target->InSaferegion()) { return 60; }
    const unsigned before = reached.load(std::memory_order_acquire);
    if (!stopped) {
        const auto result = callback();
        const bool ok = result == 0 && reached.load(std::memory_order_acquire) == before + 1;
        std::fprintf(stderr, "R06_CONTROL reached=%u ok=%d\n", reached.load(), ok);
        return ok ? 0 : 61;
    }
    auto& manager = MutatorManager::Instance();
    std::atomic<bool> ready{false}, finished{false};
    std::atomic<int> status{62};
    std::thread requester([&] {
        manager.StopTheWorld();
        const bool published = manager.WorldStopped() &&
            target->HasSuspensionRequest(Mutator::SUSPENSION_FOR_SYNC);
        ready.store(true, std::memory_order_release);
        // The existing SuspendForSync clears SYNC before waiting. Observing
        // that clear proves the actual N2C ingress reached product handling.
        const bool arrived = Wait([&] {
            return !target->HasSuspensionRequest(Mutator::SUSPENSION_FOR_SYNC);
        });
        const bool held = reached.load(std::memory_order_acquire) == before;
        const bool active = manager.WorldStopped();
        std::fprintf(stderr, "R06_STOP_ASSERT published=%d arrived=%d held=%d active=%d\n",
                     published, arrived, held, active);
        status.store(published && arrived && held && active ? 0 : 63);
        manager.StartTheWorld();
        finished.store(true, std::memory_order_release);
    });
    const bool readyOk = Wait([&] { return ready.load(std::memory_order_acquire); });
    // Even on a failed precondition, join after the requester's bounded wait
    // and release rather than abandoning a live STW producer.
    const auto result = readyOk ? callback() : -1;
    requester.join();
    const bool ok = readyOk && finished.load(std::memory_order_acquire) && status.load() == 0 &&
        result == 0 && reached.load(std::memory_order_acquire) == before + 1;
    std::fprintf(stderr, "R06_RESUME_ASSERT reached=%u finished=%d ok=%d\n",
                 reached.load(), finished.load(), ok);
    return ok ? 0 : 64;
}
