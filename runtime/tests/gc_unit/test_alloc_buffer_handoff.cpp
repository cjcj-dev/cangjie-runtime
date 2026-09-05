// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Mutator-local publication buffers vs the concurrent young-mark consumer.
//
// WCollector::MarkYoungSatbBuffer runs with mutators live under TraceBarrier
// (Mark.cpp:2192-2196 comment; the pause is only entered later at
// Generation.cpp:1136).  Inside it, Mark.cpp:2241-2249 walks every AllocBuffer
// and drains four mutator-owned containers.  Only the AllocBufferManager set
// itself is locked (AllocBufferManager.h:52-59); the containers are not.
//
// ZGC keeps the mutator's local store-barrier buffer owned by the mutator and
// only takes it at a handshake -- ZStoreBarrierBuffer::on_new_phase installs a
// fresh buffer and the old one is published, it is never iterated and cleared
// underneath a running mutator (zStoreBarrierBuffer.cpp:104-120).
//
// Invariant under test: an object a mutator publishes while the consumer is
// draining must still be delivered in some batch.  It may land in this batch or
// the next one; it must not be dropped, and publication must not mutate the
// container the consumer is walking.

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "Common/Runtime.h"
#include "CjScheduler.h"

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Collector/MarkStackEntry.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

#if defined(MRT_GC_UNIT_TESTS)
namespace {

// The consumer stands in for the GC thread at Mark.cpp:2242-2243.  The hook
// body is the linearisation of "a mutator ran here": it performs exactly the
// product publication call a live mutator would make at that instant.
struct LatePublication {
    AllocBuffer* buffer{ nullptr };
    BaseObject* late{ nullptr };
    bool fired{ false };
};

void PublishRootDuringMerge(void* context)
{
    auto& pub = *static_cast<LatePublication*>(context);
    if (pub.fired) {
        return;
    }
    pub.fired = true;
    pub.buffer->PushRoot(pub.late);
}

void PublishAllocBlackDuringMerge(void* context)
{
    auto& pub = *static_cast<LatePublication*>(context);
    if (pub.fired) {
        return;
    }
    pub.fired = true;
    pub.buffer->PushYoungAllocBlack(pub.late);
}

size_t CountEntry(const std::vector<MarkStackEntry>& stack, BaseObject* obj)
{
    size_t seen = 0;
    for (const MarkStackEntry& entry : stack) {
        if (entry.object() == obj) {
            ++seen;
        }
    }
    return seen;
}

// Two live threads on the same container.  The consumer releases the producer
// at the instant it is about to retire the batch, so the producer's
// emplace_back overlaps the consumer's clear() on the same std::list.
struct BurstGate {
    std::mutex lock;
    std::condition_variable changed;
    bool consumerAtRetire{ false };
};

void ReleaseBurstAtRetire(void* context)
{
    auto& gate = *static_cast<BurstGate*>(context);
    std::lock_guard<std::mutex> guard(gate.lock);
    gate.consumerAtRetire = true;
    gate.changed.notify_all();
}

constexpr size_t kBurst = 200000;

} // namespace

// Row 1: AllocBuffer::stackRoots.  Producer Mutator.cpp:940 (PushRoot ->
// AllocBuffer.h:55).  Consumer Mark.cpp:2242 (MergeRoots -> AllocBuffer.h:63).
// A root dropped here is a live object the young closure never reaches.
GC_TEST(AllocBufferHandoff, StackRootPublishedDuringMergeIsDelivered)
{
    GcHeapFixture fx;
    AllocBuffer* bufferOwner = new AllocBuffer();
    AllocBuffer& buffer = *bufferOwner;
    LatePublication pub{ &buffer, fx.obj1, false };

    buffer.PushRoot(fx.obj0);
    buffer.SetStackRootsHandoffHookForTest(PublishRootDuringMerge, &pub);

    std::vector<MarkStackEntry> firstBatch;
    buffer.MergeRoots(firstBatch);
    buffer.SetStackRootsHandoffHookForTest(nullptr, nullptr);
    GC_EXPECT_TRUE(pub.fired);

    std::vector<MarkStackEntry> secondBatch;
    buffer.MergeRoots(secondBatch);

    GC_EXPECT_EQ(CountEntry(firstBatch, fx.obj0), 1u);
    // obj1 was published while the consumer owned the buffer.  Either batch is
    // a legal place for it; being in neither means the clear() dropped it.
    GC_EXPECT_EQ(CountEntry(firstBatch, fx.obj1) + CountEntry(secondBatch, fx.obj1), 1u);
}

// Row 2: AllocBuffer::youngAllocBlack.  Producer Barrier.cpp:436 and
// RegionSpace.cpp:389 (PushYoungAllocBlack -> AllocBuffer.h:76).  Consumer
// Mark.cpp:2243 (MergeYoungAllocBlackFollow -> AllocBuffer.h:94).  Allocate-black
// has already claimed the mark bit, so a dropped entry is an object whose
// children are never traced.
GC_TEST(AllocBufferHandoff, AllocBlackPublishedDuringMergeIsDelivered)
{
    GcHeapFixture fx;
    AllocBuffer* bufferOwner = new AllocBuffer();
    AllocBuffer& buffer = *bufferOwner;
    LatePublication pub{ &buffer, fx.obj1, false };

    buffer.PushYoungAllocBlack(fx.obj0);
    buffer.SetYoungAllocBlackHandoffHookForTest(PublishAllocBlackDuringMerge, &pub);

    std::vector<MarkStackEntry> firstBatch;
    buffer.MergeYoungAllocBlackFollow(firstBatch);
    buffer.SetYoungAllocBlackHandoffHookForTest(nullptr, nullptr);
    GC_EXPECT_TRUE(pub.fired);

    std::vector<MarkStackEntry> secondBatch;
    buffer.MergeYoungAllocBlackFollow(secondBatch);

    GC_EXPECT_EQ(CountEntry(firstBatch, fx.obj0), 1u);
    GC_EXPECT_EQ(CountEntry(firstBatch, fx.obj1) + CountEntry(secondBatch, fx.obj1), 1u);
}

// The allocator-level consequence of the same window.  std::list::clear() walks
// _M_next and frees each node; emplace_back links a node onto the tail it read.
// Overlapping them frees a pointer read from a torn chain, which is what glibc
// reports as an unaligned fastbin chunk.  Other-VM so a glibc abort is this
// test's failure and not the suite's.
GC_OTHER_VM_TEST(AllocBufferHandoff, AllocBlackPublishDuringRetireKeepsHeapIntact)
{
    GcHeapFixture fx;
    AllocBuffer* bufferOwner = new AllocBuffer();
    AllocBuffer& buffer = *bufferOwner;
    BurstGate gate;

    for (size_t i = 0; i < kBurst; ++i) {
        buffer.PushYoungAllocBlack(fx.obj0);
    }
    buffer.SetYoungAllocBlackHandoffHookForTest(ReleaseBurstAtRetire, &gate);

    std::thread producer([&buffer, &gate, &fx]() {
        {
            std::unique_lock<std::mutex> lock(gate.lock);
            gate.changed.wait(lock, [&gate]() { return gate.consumerAtRetire; });
        }
        for (size_t i = 0; i < kBurst; ++i) {
            buffer.PushYoungAllocBlack(fx.obj1);
        }
    });
    JoinGuard join(producer);

    std::vector<MarkStackEntry> batch;
    buffer.MergeYoungAllocBlackFollow(batch);
    producer.join();
    buffer.SetYoungAllocBlackHandoffHookForTest(nullptr, nullptr);

    std::vector<MarkStackEntry> drain;
    buffer.MergeYoungAllocBlackFollow(drain);

    // Every published object must be accounted for exactly once across batches.
    GC_EXPECT_EQ(CountEntry(batch, fx.obj0) + CountEntry(drain, fx.obj0), kBurst);
    GC_EXPECT_EQ(CountEntry(batch, fx.obj1) + CountEntry(drain, fx.obj1), kBurst);
}

// Same shape on stackRoots, whose node carries a MarkStackEntry rather than a
// bare pointer, so it lands in a different glibc size class.
GC_OTHER_VM_TEST(AllocBufferHandoff, StackRootPublishDuringRetireKeepsHeapIntact)
{
    GcHeapFixture fx;
    AllocBuffer* bufferOwner = new AllocBuffer();
    AllocBuffer& buffer = *bufferOwner;
    BurstGate gate;

    for (size_t i = 0; i < kBurst; ++i) {
        buffer.PushRoot(fx.obj0);
    }
    buffer.SetStackRootsHandoffHookForTest(ReleaseBurstAtRetire, &gate);

    std::thread producer([&buffer, &gate, &fx]() {
        {
            std::unique_lock<std::mutex> lock(gate.lock);
            gate.changed.wait(lock, [&gate]() { return gate.consumerAtRetire; });
        }
        for (size_t i = 0; i < kBurst; ++i) {
            buffer.PushRoot(fx.obj1);
        }
    });
    JoinGuard join(producer);

    std::vector<MarkStackEntry> batch;
    buffer.MergeRoots(batch);
    producer.join();
    buffer.SetStackRootsHandoffHookForTest(nullptr, nullptr);

    std::vector<MarkStackEntry> drain;
    buffer.MergeRoots(drain);

    GC_EXPECT_EQ(CountEntry(batch, fx.obj0) + CountEntry(drain, fx.obj0), kBurst);
    GC_EXPECT_EQ(CountEntry(batch, fx.obj1) + CountEntry(drain, fx.obj1), kBurst);
}
#endif // MRT_GC_UNIT_TESTS
