// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <memory>
#include <mutex>
#include <unordered_map>
#include "Heap/Collector/MarkStripe.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
MarkStackEntry Entry(size_t i)
{
    return MarkStackEntry::PartialArray(i + 1, i + 3, (i & 1) != 0);
}
void ExpectEntry(const MarkStackEntry& entry, size_t i)
{
    GC_EXPECT_TRUE(entry.partialArray());
    GC_EXPECT_EQ(entry.partialArrayOffset(), i + 1);
    GC_EXPECT_EQ(entry.partialArrayLength(), i + 3);
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
    MarkingSMR smr(1);
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
        MarkingSMR smr(1);
        MarkThreadLocalStacks producer(4);
        MarkThreadLocalStacks consumer(4);
        constexpr size_t count = 128 + 512 + 7;
        for (size_t s = 0; s < 4; ++s) {
            for (size_t i = 0; i < count; ++i) {
                producer.Push(stripes, s, Entry(s * count + i), publish);
            }
        }
        GC_EXPECT_TRUE(producer.Flush(stripes, publish));
        GC_EXPECT_TRUE(producer.IsEmpty());
        for (size_t s = 0; s < 4; ++s) {
            std::vector<bool> seen(count, false);
            size_t popped = 0;
            MarkStackEntry entry;
            while (consumer.Pop(smr, 0, stripes, s, entry)) {
                const size_t i = entry.partialArrayOffset() - 1 - s * count;
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
    MarkingSMR smr(1);
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

#if defined(MRT_TESTABLE_INTERNALS)
namespace {
struct StorageEvent {
    uintptr_t stack;
    uintptr_t data;
    size_t capacity;
    bool allocated;
};
std::mutex eventLock;
std::vector<StorageEvent> events;
void ObserveStorage(const MarkStripeStack* stack, const MarkStackEntry* data, size_t capacity, bool allocated)
{
    std::lock_guard<std::mutex> lock(eventLock);
    events.push_back({reinterpret_cast<uintptr_t>(stack), reinterpret_cast<uintptr_t>(data), capacity, allocated});
}
struct ObserveScope {
    ObserveScope()
    {
        events.clear();
        MarkStripeStack::SetStorageObserver(ObserveStorage);
    }
    ~ObserveScope() { MarkStripeStack::SetStorageObserver(nullptr); }
};
void ExpectPairedStorage()
{
    std::unordered_map<uintptr_t, StorageEvent> live;
    size_t allocations = 0;
    for (const auto& event : events) {
        std::fprintf(stderr, "STORAGE_EVENT stack=%zx data=%zx capacity=%zu allocated=%d\n",
                     event.stack, event.data, event.capacity, event.allocated);
        if (event.allocated) {
            GC_EXPECT_TRUE(live.emplace(event.stack, event).second);
            ++allocations;
            using Attached = ZAttachedArray<MarkStripeStack, MarkStackEntry>;
            GC_EXPECT_EQ(event.data, event.stack + Attached::object_size());
            GC_EXPECT_EQ(event.data % alignof(MarkStackEntry), 0u);
        } else {
            auto found = live.find(event.stack);
            GC_EXPECT_TRUE(found != live.end());
            GC_EXPECT_EQ(found->second.data, event.data);
            GC_EXPECT_EQ(found->second.capacity, event.capacity);
            live.erase(found);
        }
    }
    std::fprintf(stderr, "STORAGE_RESULT allocations=%zu outstanding=%zu\n", allocations, live.size());
    GC_EXPECT_TRUE(allocations > 0);
    GC_EXPECT_EQ(live.size(), 0u);
}
}

GC_TEST(MarkPort203Storage, EmptySingleAndMultiplePrivateSegmentsAreReturned)
{
    ObserveScope observe;
    MarkStripeStack::Destroy(nullptr);
    MarkStripeStack::Destroy(MarkStripeStack::Create(true));
    MarkStripeSet stripes(4);
    {
        MarkThreadLocalStacks local(4);
        for (size_t s = 0; s < 4; ++s) {
            local.Push(stripes, s, Entry(s), true);
        }
    }
    ExpectPairedStorage();
}

GC_OTHER_VM_TEST(MarkPort203Storage, YoungCollectionReturnsActualSegments)
{
    // Reuse the registered real-heap fixture, including its object/weak-edge
    // assertions and DoGarbageCollection entry. Do not recreate a mark engine
    // or manually pass entries between product stages in this integration arm.
    void (*runFixture)() = nullptr;
    for (const auto& test : Registry()) {
        if (std::strcmp(test.suite, "YoungWeakClosure") == 0 &&
            std::strcmp(test.name, "StripedDiscoversWithoutStrongReferentClosure") == 0) {
            runFixture = test.fn;
        }
    }
    GC_EXPECT_TRUE(runFixture != nullptr);
    ObserveScope observe;
    std::exception_ptr fixtureFailure;
    try {
        runFixture();
    } catch (...) {
        fixtureFailure = std::current_exception();
    }
    // Always execute the storage invariant, even if an entry cut also made
    // the fixture's own earlier closure assertion fail.
    ExpectPairedStorage();
    if (fixtureFailure) {
        std::rethrow_exception(fixtureFailure);
    }
}
#endif
