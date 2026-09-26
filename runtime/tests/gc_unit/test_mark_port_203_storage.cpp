// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_worker_fixture.hpp"
#include <memory>
#include <mutex>
#include <unordered_map>
#include "Heap/z/zMarkStack.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
MarkStackEntry Entry(size_t i)
{
    return MarkStackEntry(size_t(i + 1), size_t(i + 3), (i & 1) != 0);
}
void ExpectEntry(const MarkStackEntry& entry, size_t i)
{
    GC_EXPECT_TRUE(entry.partial_array());
    GC_EXPECT_EQ(entry.partial_array_offset(), i + 1);
    GC_EXPECT_EQ(entry.partial_array_length(), i + 3);
    GC_EXPECT_EQ(entry.finalizable(), (i & 1) != 0);
}
using StackOwner = std::unique_ptr<MarkStripeStack, decltype(&MarkStripeStack::Destroy)>;
}

GC_TEST(MarkPort203Storage, FirstAndRegularCapacityPreserveValidPrefix)
{
    for (bool first : {true, false}) {
        StackOwner stack(MarkStripeStack::Create(first), MarkStripeStack::Destroy);
        GC_EXPECT_TRUE(stack != nullptr);
        GC_EXPECT_EQ(stack->Capacity(), first ? 128u : 512u);
        GC_EXPECT_TRUE(stack->IsEmpty());
        for (size_t i = 0; i < stack->Capacity(); ++i) {
            stack->Push(Entry(i));
        }
        GC_EXPECT_TRUE(stack->IsFull());
        for (size_t i = stack->Capacity(); i > 0; --i) {
            ExpectEntry(stack->Pop(), i - 1);
        }
        GC_EXPECT_TRUE(stack->IsEmpty());
        stack->Push(Entry(777));
        ExpectEntry(stack->Pop(), 777);
    }
}

GC_TEST(MarkPort203Storage, FullFirstPublishesThenUsesRegularSegment)
{
    MarkStripeSet stripes(1);
    MarkThreadLocalStacks local(1);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    for (size_t i = 0; i < 129; ++i) {
        local.Push(stripes, 0, Entry(i), false);
    }
    StackOwner next(local.StealLocal(0), MarkStripeStack::Destroy);
    StackOwner first(stripes.At(0).StealStack(smr, 0), MarkStripeStack::Destroy);
    GC_EXPECT_TRUE(next != nullptr && first != nullptr);
    GC_EXPECT_EQ(next->Capacity(), 512u);
    GC_EXPECT_EQ(first->Capacity(), 128u);
    GC_EXPECT_EQ(first->Size(), 128u);
    ExpectEntry(next->Pop(), 128);
    for (size_t i = 128; i > 0; --i) {
        ExpectEntry(first->Pop(), i - 1);
    }
}

GC_TEST(MarkPort203Storage, BothPublicationListsDrainMultipleStripesAndSegments)
{
    for (bool publish : {true, false}) {
        MarkStripeSet stripes(4);
        MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
        MarkThreadLocalStacks producer(4);
        MarkThreadLocalStacks consumer(4);
        constexpr size_t count = 128 + 512 + 7;
        for (size_t s = 0; s < 4; ++s) {
            for (size_t i = 0; i < count; ++i) {
                producer.Push(stripes, s, Entry(s * count + i), publish);
            }
        }
        GC_EXPECT_TRUE(producer.Flush(stripes));
        GC_EXPECT_TRUE(producer.IsEmpty());
        for (size_t s = 0; s < 4; ++s) {
            std::vector<bool> seen(count, false);
            size_t popped = 0;
            MarkStackEntry entry;
            while (consumer.Pop(smr, 0, stripes, s, entry)) {
                const size_t i = entry.partial_array_offset() - 1 - s * count;
                GC_EXPECT_TRUE(i < count);
                GC_EXPECT_FALSE(seen[i]);
                seen[i] = true;
                ExpectEntry(entry, s * count + i);
                ++popped;
            }
            GC_EXPECT_EQ(popped, count);
        }
        GC_EXPECT_TRUE(consumer.IsEmpty());
        GC_EXPECT_TRUE(stripes.IsEmpty());
    }
}

GC_TEST(MarkPort203Storage, TransferredSegmentOutlivesItsSource)
{
    MarkStripeSet stripes(1);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    MarkThreadLocalStacks destination(1);
    {
        MarkThreadLocalStacks source(1);
        source.Push(stripes, 0, Entry(9), true);
        destination.Install(0, source.StealLocal(0));
        GC_EXPECT_TRUE(source.IsEmpty());
    }
    MarkStackEntry entry;
    GC_EXPECT_TRUE(destination.Pop(smr, 0, stripes, 0, entry));
    ExpectEntry(entry, 9);
    GC_EXPECT_FALSE(destination.Pop(smr, 0, stripes, 0, entry));
}
