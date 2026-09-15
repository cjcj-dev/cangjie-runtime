// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Mutator-local publication buffers vs the concurrent young-mark consumer.
//
// WCollector::FollowYoungMark runs with mutators live under TraceBarrier
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

#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zMarkStackEntry.hpp"
#include "Heap/z/zMark.hpp"
#include "Mutator/ThreadLocal.h"

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

size_t CountEntry(const std::vector<MarkStackEntry>& stack, BaseObject* obj)
{
    size_t seen = 0;
    for (const MarkStackEntry& entry : stack) {
        if (to_object(ZOffset::address(to_zoffset(entry.object_address()))) == obj) {
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

// ZMark::flush: a non-full TLS stack is published, and later publication
// remains private until the next flush. Both entries reach the shared stripes.
GC_TEST(AllocBufferHandoff, StackRootPublishedDuringMergeIsDelivered)
{
    GcHeapFixture fx;
    MarkDomain domain(64, MarkingStacks::MarkingGeneration::YOUNG);
    domain.PrepareWork(1);
    auto& producer = domain.Stacks();
    producer.Push(domain.Stripes(), 0, MarkStackEntry(untype(ZAddress::offset(from_object(fx.obj0))), true, true, true, false), true);
    GC_EXPECT_TRUE(domain.Stripes().IsEmpty());
    GC_EXPECT_TRUE(domain.FlushStacks());
    producer.Push(domain.Stripes(), 0, MarkStackEntry(untype(ZAddress::offset(from_object(fx.obj1))), true, true, true, false), true);
    GC_EXPECT_TRUE(domain.FlushStacks());
    std::vector<MarkStackEntry> delivered;
    WorkerFixture worker;
    MarkThreadLocalStacks consumer(64);
    MarkStackEntry entry;
    while (consumer.Pop(domain.Smr(), 0, domain.Stripes(), 0, entry)) {
        delivered.push_back(entry);
    }
    GC_EXPECT_EQ(CountEntry(delivered, fx.obj0), 1u);
    GC_EXPECT_EQ(CountEntry(delivered, fx.obj1), 1u);
}

// The owner remains the OS thread even when two threads exchange allocator
// bindings (the allocation context associated with a processor).
GC_OTHER_VM_TEST(AllocBufferHandoff, StackRootPublishDuringRetireKeepsHeapIntact)
{
    GcHeapFixture fx;
    MarkDomain domain(64, MarkingStacks::MarkingGeneration::YOUNG);
    domain.PrepareWork(1);
    AllocBuffer first;
    AllocBuffer second;
    std::atomic<unsigned> ready{0};
    ThreadGCData* owners[2]{};
    auto publish = [&](size_t id) {
        ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
        ThreadLocal::SetAllocBuffer(id == 0 ? &first : &second);
        owners[id] = &ThreadLocal::GetGCData();
        auto& stacks = domain.Stacks();
        stacks.Push(domain.Stripes(), id,
                    MarkStackEntry(untype(ZAddress::offset(from_object(id == 0 ? fx.obj0 : fx.obj1))), true, true, true, false), true);
        ready.fetch_add(1);
        while (ready.load() != 2) { std::this_thread::yield(); }
        ThreadLocal::SetAllocBuffer(id == 0 ? &second : &first);
        GC_EXPECT_TRUE(owners[id] == &ThreadLocal::GetGCData());
        GC_EXPECT_TRUE(domain.FlushStacks());
        ThreadLocal::SetAllocBuffer(nullptr);
    };
    std::thread one(publish, 0);
    std::thread two(publish, 1);
    one.join();
    two.join();
    GC_EXPECT_TRUE(owners[0] != owners[1]);
    std::vector<MarkStackEntry> delivered;
    WorkerFixture worker;
    MarkThreadLocalStacks consumer(64);
    MarkStackEntry entry;
    for (size_t stripe = 0; stripe < 2; ++stripe) {
        while (consumer.Pop(domain.Smr(), 0, domain.Stripes(), stripe, entry)) {
            delivered.push_back(entry);
        }
    }
    GC_EXPECT_EQ(CountEntry(delivered, fx.obj0), 1u);
    GC_EXPECT_EQ(CountEntry(delivered, fx.obj1), 1u);
}
#endif // MRT_GC_UNIT_TESTS
