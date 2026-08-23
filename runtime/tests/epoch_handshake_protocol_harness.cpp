// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Models the GC-assist epoch-handshake publication protocol.
// PROTOCOL_FIXED=0 is the hunt-mut tree; PROTOCOL_FIXED=1 is the repaired order.
//
// usage: epoch_handshake_protocol_harness <park|resume|exit|ack_order>

#ifndef PROTOCOL_FIXED
#error "PROTOCOL_FIXED must be 0 (old) or 1 (fixed)"
#endif

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

enum Lifecycle : uint32_t {
    LC_STARTING = 0,
    LC_RUNNING = 1,
    LC_PARKED = 2,
    LC_EXITING = 3,
};

enum HandshakeState : uint32_t {
    HS_IDLE = 0,
    HS_REQUESTED = 1,
    HS_CLAIMED = 2,
    HS_ACKNOWLEDGED = 3,
};

constexpr uint32_t SAFE_TRUE = 0x17161514;
constexpr uint32_t SAFE_FALSE = 0x03020100;

struct MutatorModel {
    std::atomic<uint32_t> inSaferegion { SAFE_TRUE };
    std::atomic<Lifecycle> lifecycle { LC_STARTING };
    std::atomic<HandshakeState> handshake { HS_IDLE };
    std::atomic<uint64_t> completion { 0 };
    std::atomic<uintptr_t> contextGen { 0 };
    std::atomic<int> assistRunningUnsafe { 0 };
    std::mutex mutatorLock;
    std::vector<int> localFinalizers;
    std::vector<int> registered;
    std::atomic<int> finalizerRaces { 0 };
};

bool InSaferegion(const MutatorModel& m)
{
    return m.inSaferegion.load(std::memory_order_seq_cst) != SAFE_FALSE;
}

bool CanGcAssist(const MutatorModel& m)
{
#if PROTOCOL_FIXED
    return InSaferegion(m);
#else
    return InSaferegion(m) || m.lifecycle.load(std::memory_order_acquire) != LC_RUNNING;
#endif
}

void WaitIfClaimed(MutatorModel& m)
{
    for (;;) {
        std::unique_lock<std::mutex> lock(m.mutatorLock);
        if (m.handshake.load(std::memory_order_acquire) == HS_CLAIMED) {
            lock.unlock();
            std::this_thread::yield();
            continue;
        }
        break;
    }
}

void PreparedToPark(MutatorModel& m, uintptr_t newGen)
{
#if PROTOCOL_FIXED
    m.contextGen.store(newGen, std::memory_order_relaxed);
    m.inSaferegion.store(SAFE_TRUE, std::memory_order_seq_cst);
    m.lifecycle.store(LC_PARKED, std::memory_order_release);
#else
    m.lifecycle.store(LC_PARKED, std::memory_order_release);
    m.contextGen.store(newGen, std::memory_order_relaxed);
    m.inSaferegion.store(SAFE_TRUE, std::memory_order_seq_cst);
#endif
}

void PreparedToRun(MutatorModel& m)
{
#if PROTOCOL_FIXED
    m.lifecycle.store(LC_RUNNING, std::memory_order_release);
    WaitIfClaimed(m);
    {
        std::lock_guard<std::mutex> lock(m.mutatorLock);
        m.inSaferegion.store(SAFE_FALSE, std::memory_order_seq_cst);
    }
#else
    m.inSaferegion.store(SAFE_FALSE, std::memory_order_seq_cst);
    m.lifecycle.store(LC_RUNNING, std::memory_order_release);
#endif
}

bool DrainAsGc(MutatorModel& m, uintptr_t* seenGen)
{
    std::lock_guard<std::mutex> lock(m.mutatorLock);
#if PROTOCOL_FIXED
    if (!InSaferegion(m)) {
        return false;
    }
#endif
    *seenGen = m.contextGen.load(std::memory_order_relaxed);
    if (m.lifecycle.load(std::memory_order_acquire) == LC_RUNNING && !InSaferegion(m)) {
        m.assistRunningUnsafe.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool Acknowledge(MutatorModel& m, uint64_t epoch, bool bySelf, uintptr_t* seenGen)
{
    HandshakeState expected = HS_REQUESTED;
    if (!m.handshake.compare_exchange_strong(expected, HS_CLAIMED, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
        return expected == HS_ACKNOWLEDGED && m.completion.load(std::memory_order_acquire) == epoch;
    }
#if PROTOCOL_FIXED
    if (!bySelf && !CanGcAssist(m)) {
        m.handshake.store(HS_REQUESTED, std::memory_order_release);
        return false;
    }
#endif
    bool scanned = DrainAsGc(m, seenGen);
#if PROTOCOL_FIXED
    m.completion.store(epoch, std::memory_order_release);
    m.handshake.store(HS_ACKNOWLEDGED, std::memory_order_release);
#else
    m.handshake.store(HS_ACKNOWLEDGED, std::memory_order_release);
    m.completion.store(epoch, std::memory_order_release);
#endif
    return scanned;
}

void ResetMutator(MutatorModel& m)
{
#if PROTOCOL_FIXED
    WaitIfClaimed(m);
    std::lock_guard<std::mutex> lock(m.mutatorLock);
#endif
    if (!m.localFinalizers.empty()) {
        m.registered.insert(m.registered.end(), m.localFinalizers.begin(), m.localFinalizers.end());
        m.localFinalizers.clear();
    }
}

void GcTakeLocalFinalizers(MutatorModel& m)
{
#if PROTOCOL_FIXED
    std::lock_guard<std::mutex> lock(m.mutatorLock);
#endif
    if (!m.localFinalizers.empty()) {
        m.registered.insert(m.registered.end(), m.localFinalizers.begin(), m.localFinalizers.end());
        m.localFinalizers.clear();
    }
}

void Expect(bool cond, const char* msg)
{
    if (!cond) {
        std::cerr << "HARNESS_FAIL " << msg << '\n';
        std::exit(1);
    }
}

int RunPark()
{
    MutatorModel m;
    m.lifecycle.store(LC_RUNNING, std::memory_order_relaxed);
    m.inSaferegion.store(SAFE_FALSE, std::memory_order_relaxed);
    m.contextGen.store(1, std::memory_order_relaxed);
    m.handshake.store(HS_REQUESTED, std::memory_order_relaxed);

    std::atomic<int> parkedPublished { 0 };
    std::atomic<int> gcSaw { 0 };
    uintptr_t seenGen = 0;

    std::thread target([&]() {
#if PROTOCOL_FIXED
        PreparedToPark(m, 2);
        parkedPublished.store(1, std::memory_order_release);
#else
        m.lifecycle.store(LC_PARKED, std::memory_order_release);
        parkedPublished.store(1, std::memory_order_release);
        while (gcSaw.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        m.contextGen.store(2, std::memory_order_relaxed);
        m.inSaferegion.store(SAFE_TRUE, std::memory_order_seq_cst);
#endif
    });

    while (parkedPublished.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    bool claimed = Acknowledge(m, 1, false, &seenGen);
    gcSaw.store(1, std::memory_order_release);
    target.join();

#if PROTOCOL_FIXED
    Expect(claimed, "fixed park must still be assistable after SAFE+context");
    Expect(seenGen == 2, "fixed park must scan the new generation");
    Expect(m.assistRunningUnsafe.load() == 0, "fixed park must not scan RUNNING&&!SAFE");
    std::cerr << "HARNESS_OK case=park seenGen=" << seenGen << '\n';
    return 0;
#else
    Expect(CanGcAssist(m) || claimed, "old park window must be assistable after PARKED");
    Expect(claimed, "old park window must let GC claim");
    Expect(seenGen == 1, "old park window must scan stale generation");
    std::cerr << "HARNESS_OK_OLD case=park seenGen=" << seenGen << '\n';
    return 0;
#endif
}

int RunResume()
{
    MutatorModel m;
    m.lifecycle.store(LC_PARKED, std::memory_order_relaxed);
    m.inSaferegion.store(SAFE_TRUE, std::memory_order_relaxed);
    m.handshake.store(HS_REQUESTED, std::memory_order_relaxed);

    std::atomic<int> leftSafe { 0 };
    std::atomic<int> gcDecided { 0 };
    uintptr_t seenGen = 0;

    std::thread target([&]() {
#if PROTOCOL_FIXED
        PreparedToRun(m);
        leftSafe.store(1, std::memory_order_release);
#else
        m.inSaferegion.store(SAFE_FALSE, std::memory_order_seq_cst);
        leftSafe.store(1, std::memory_order_release);
        while (gcDecided.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        m.lifecycle.store(LC_RUNNING, std::memory_order_release);
#endif
    });

    while (leftSafe.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    bool assistable = CanGcAssist(m);
    bool claimed = false;
    if (assistable) {
        claimed = Acknowledge(m, 1, false, &seenGen);
    }
    gcDecided.store(1, std::memory_order_release);
    target.join();

#if PROTOCOL_FIXED
    Expect(!assistable, "fixed resume must not be assistable after leave");
    Expect(!claimed, "fixed resume must not let GC claim a running stack");
    Expect(m.assistRunningUnsafe.load() == 0, "fixed resume must not scan RUNNING&&!SAFE");
    std::cerr << "HARNESS_OK case=resume assistable=0\n";
    return 0;
#else
    Expect(assistable, "old resume window is assistable from stale PARKED");
    Expect(claimed, "old resume window lets GC claim");
    std::cerr << "HARNESS_OK_OLD case=resume assistable=1 claimed=1 unsafe="
              << m.assistRunningUnsafe.load() << '\n';
    return 0;
#endif
}

int RunExit()
{
    MutatorModel m;
    m.localFinalizers.push_back(7);
    std::atomic<int> ready { 0 };
    std::atomic<int> snapGo { 0 };
    std::atomic<int> snaps { 0 };

    auto take = [&]() {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (snapGo.load(std::memory_order_acquire) == 0) {
        }
#if PROTOCOL_FIXED
        std::lock_guard<std::mutex> lock(m.mutatorLock);
#endif
        std::vector<int> snap = m.localFinalizers;
        snaps.fetch_add(1, std::memory_order_acq_rel);
#if PROTOCOL_FIXED
        m.localFinalizers.clear();
#else
        while (snaps.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
        m.localFinalizers.clear();
#endif
        m.registered.insert(m.registered.end(), snap.begin(), snap.end());
    };

    std::thread gc(take);
    std::thread owner(take);
    while (ready.load(std::memory_order_acquire) < 2) {
    }
    snapGo.store(1, std::memory_order_release);
    gc.join();
    owner.join();

    int count = static_cast<int>(m.registered.size());
#if PROTOCOL_FIXED
    Expect(count == 1, "fixed exit must register the local finalizer once");
    Expect(m.localFinalizers.empty(), "fixed exit must empty the mutator list");
    std::cerr << "HARNESS_OK case=exit registered=" << count << '\n';
    return 0;
#else
    Expect(count == 2, "old exit must double-handoff the unlocked list");
    std::cerr << "HARNESS_OK_OLD case=exit registered=" << count << '\n';
    return 0;
#endif
}

int RunAckOrder()
{
    MutatorModel m;
    m.handshake.store(HS_CLAIMED, std::memory_order_relaxed);
    std::atomic<int> mid { 0 };
    std::atomic<int> observed { 0 };
    HandshakeState seenState = HS_IDLE;
    uint64_t seenDone = 0;

    std::thread observer([&]() {
        while (mid.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        seenState = m.handshake.load(std::memory_order_acquire);
        seenDone = m.completion.load(std::memory_order_acquire);
        observed.store(1, std::memory_order_release);
    });

#if PROTOCOL_FIXED
    m.completion.store(1, std::memory_order_release);
    mid.store(1, std::memory_order_release);
    while (observed.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    m.handshake.store(HS_ACKNOWLEDGED, std::memory_order_release);
#else
    m.handshake.store(HS_ACKNOWLEDGED, std::memory_order_release);
    mid.store(1, std::memory_order_release);
    while (observed.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    m.completion.store(1, std::memory_order_release);
#endif
    observer.join();

#if PROTOCOL_FIXED
    Expect(!(seenState == HS_ACKNOWLEDGED && seenDone != 1),
           "fixed ack must not expose ACK without completion");
    std::cerr << "HARNESS_OK case=ack_order state=" << static_cast<unsigned>(seenState)
              << " done=" << seenDone << '\n';
    return 0;
#else
    Expect(seenState == HS_ACKNOWLEDGED && seenDone != 1,
           "old ack must expose ACK without completion");
    std::cerr << "HARNESS_OK_OLD case=ack_order forbidden=1 state="
              << static_cast<unsigned>(seenState) << " done=" << seenDone << '\n';
    return 0;
#endif
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: epoch_handshake_protocol_harness park|resume|exit|ack_order\n";
        return 2;
    }
    if (std::strcmp(argv[1], "park") == 0) {
        return RunPark();
    }
    if (std::strcmp(argv[1], "resume") == 0) {
        return RunResume();
    }
    if (std::strcmp(argv[1], "exit") == 0) {
        return RunExit();
    }
    if (std::strcmp(argv[1], "ack_order") == 0) {
        return RunAckOrder();
    }
    std::cerr << "unknown case " << argv[1] << '\n';
    return 2;
}
