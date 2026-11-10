// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// HotSpot handshake.cpp:507-601 over utilities/filterQueue.inline.hpp:33-138:
// lock-free adds against serialized peek/pop on the product HandshakeState.

#include "gc_unittest.hpp"

#include "CangjieRuntime.h"
#include "Common/Runtime.h"
#include "Mutator/Handshake.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {
using namespace MapleRuntime;

class FilterQueueHost final : public Runtime {
public:
    FilterQueueHost()
    {
        runtime = this;
        mutatorManager = &manager;
        manager.Init();
        tls = ThreadLocal::GetThreadLocalData();
        savedPoll = tls->GetPollWord();
        manager.RegisterMarkFlushThread(tls);
    }
    ~FilterQueueHost() override
    {
        manager.UnregisterMarkFlushThread(tls);
        tls->SetPollWord(savedPoll);
        runtime = nullptr;
    }
    RuntimeParam GetRuntimeParam() const override { return {}; }
    void SetGCThreshold(uint64_t) override {}

private:
    MutatorManager manager;
    ThreadLocalData* tls = nullptr;
    uintptr_t savedPoll = 0;
};

class Sequence final : public HandshakeClosure {
public:
    Sequence(std::atomic<uint64_t>& next, uint64_t seq, std::atomic<uint64_t>& disorder,
             std::atomic<uint64_t>& executed)
        : HandshakeClosure("filter-queue-sequence"), next_(next), seq_(seq), disorder_(disorder),
          executed_(executed) {}
    void do_thread(Mutator*) override
    {
        if (next_.load(std::memory_order_relaxed) != seq_) {
            disorder_.fetch_add(1, std::memory_order_relaxed);
        }
        next_.store(seq_ + 1, std::memory_order_relaxed);
        executed_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t>& next_;
    const uint64_t seq_;
    std::atomic<uint64_t>& disorder_;
    std::atomic<uint64_t>& executed_;
};
} // namespace

GC_TEST(HandshakeFilterQueue, FifoAcrossEmptyAndNonEmpty)
{
    FilterQueueHost host;
    HandshakeState state(nullptr);
    std::atomic<uint64_t> next { 0 };
    std::atomic<uint64_t> disorder { 0 };
    std::atomic<uint64_t> executed { 0 };
    bool good = !state.has_operation() && !state.try_process();
    for (uint64_t round = 0; round < 3; ++round) {
        Sequence first(next, round * 3, disorder, executed);
        Sequence second(next, round * 3 + 1, disorder, executed);
        Sequence third(next, round * 3 + 2, disorder, executed);
        HandshakeOperation a(&first, nullptr);
        HandshakeOperation b(&second, nullptr);
        HandshakeOperation c(&third, nullptr);
        state.add_operation(&a);
        state.add_operation(&b);
        state.add_operation(&c);
        good = good && state.has_operation() && state.operation_pending(&a) && state.operation_pending(&b) &&
               state.operation_pending(&c);
        // Oldest first: a and b leave through the interior unlink, c through the head CAS.
        good = good && state.try_process() && a.is_completed() && !b.is_completed() && !c.is_completed();
        good = good && !state.operation_pending(&a) && state.operation_pending(&b) && state.has_operation();
        good = good && state.try_process() && b.is_completed() && !c.is_completed();
        good = good && state.try_process() && c.is_completed() && !state.operation_pending(&c);
        good = good && !state.has_operation() && !state.try_process();
    }
    std::fprintf(stderr, "FILTER_QUEUE_FIFO good=%d executed=%lu disorder=%lu\n", good,
                 static_cast<unsigned long>(executed.load()), static_cast<unsigned long>(disorder.load()));
    GC_EXPECT_TRUE(good);
    GC_EXPECT_EQ(executed.load(), static_cast<uint64_t>(9));
    GC_EXPECT_EQ(disorder.load(), static_cast<uint64_t>(0));
}

// Each producer waits for its batch, so the queue keeps draining to empty
// while other producers publish a new head: the pop of the last node must
// lose its CAS to a concurrent push and restart instead of dropping it.
GC_TEST(HandshakeFilterQueue, ConcurrentAddAgainstLastRemove)
{
    FilterQueueHost host;
    HandshakeState state(nullptr);
    constexpr size_t producers = 4;
    constexpr uint64_t batches = 5000;
    constexpr uint64_t batchSize = 3;
    std::atomic<uint64_t> next[producers];
    for (auto& value : next) { value.store(0); }
    std::atomic<uint64_t> disorder { 0 };
    std::atomic<uint64_t> executed { 0 };
    std::atomic<uint64_t> notPending { 0 };
    std::atomic<size_t> finished { 0 };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);

    std::vector<std::thread> threads;
    for (size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            for (uint64_t batch = 0; batch < batches; ++batch) {
                std::unique_ptr<Sequence> closures[batchSize];
                std::unique_ptr<HandshakeOperation> ops[batchSize];
                for (uint64_t i = 0; i < batchSize; ++i) {
                    closures[i] = std::make_unique<Sequence>(next[p], batch * batchSize + i, disorder, executed);
                    ops[i] = std::make_unique<HandshakeOperation>(closures[i].get(), nullptr);
                    state.add_operation(ops[i].get());
                }
                HandshakeOperation& last = *ops[batchSize - 1];
                if ((batch & 63) == 0 && !last.is_completed() && !state.operation_pending(&last) &&
                    !last.is_completed()) {
                    notPending.fetch_add(1, std::memory_order_relaxed);
                }
                while (!last.is_completed()) {
                    if (std::chrono::steady_clock::now() > deadline) {
                        std::fprintf(stderr, "FILTER_QUEUE_LOST_OPERATION producer=%zu batch=%lu\n", p,
                                     static_cast<unsigned long>(batch));
                        std::abort();
                    }
                    std::this_thread::yield();
                }
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }
    // The handshakee side and a remote handshaker both serialize on lock_.
    std::thread self([&] {
        while (finished.load(std::memory_order_acquire) != producers) {
            if (state.has_operation()) { (void)state.process_by_self(); }
        }
    });
    while (finished.load(std::memory_order_acquire) != producers) {
        (void)state.try_process();
    }
    for (auto& thread : threads) { thread.join(); }
    self.join();

    const uint64_t total = producers * batches * batchSize;
    std::fprintf(stderr, "FILTER_QUEUE_CONCURRENT executed=%lu total=%lu disorder=%lu notPending=%lu pending=%d\n",
                 static_cast<unsigned long>(executed.load()), static_cast<unsigned long>(total),
                 static_cast<unsigned long>(disorder.load()), static_cast<unsigned long>(notPending.load()),
                 state.has_operation());
    GC_EXPECT_EQ(executed.load(), total);
    GC_EXPECT_EQ(disorder.load(), static_cast<uint64_t>(0));
    GC_EXPECT_EQ(notPending.load(), static_cast<uint64_t>(0));
    GC_EXPECT_FALSE(state.has_operation());
    GC_EXPECT_FALSE(state.try_process());
}

// handshake.cpp:507-513: destruction drains the nodes; synchronous operations
// stay owned by their requester and are neither run nor released.
GC_TEST(HandshakeFilterQueue, DestructionLeavesRequesterOwnedOperations)
{
    FilterQueueHost host;
    std::atomic<uint64_t> next { 0 };
    std::atomic<uint64_t> disorder { 0 };
    std::atomic<uint64_t> executed { 0 };
    Sequence first(next, 0, disorder, executed);
    Sequence second(next, 1, disorder, executed);
    HandshakeOperation a(&first, nullptr);
    HandshakeOperation b(&second, nullptr);
    bool queued = false;
    {
        HandshakeState state(nullptr);
        state.add_operation(&a);
        state.add_operation(&b);
        queued = state.has_operation() && state.operation_pending(&a) && state.operation_pending(&b);
    }
    GC_EXPECT_TRUE(queued);
    GC_EXPECT_FALSE(a.is_completed());
    GC_EXPECT_FALSE(b.is_completed());
    GC_EXPECT_EQ(executed.load(), static_cast<uint64_t>(0));
    GC_EXPECT_TRUE(a.closure() == &first && b.closure() == &second);
}
