// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Product partial-array chunking must visit each input slot exactly once.
// ZGC zMark.cpp:208-270: follow the leading range and consume published tails.

#include <cstdint>
#include <csignal>
#include <cstring>
#include <set>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <sstream>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <list>
#define private public
#include "Heap/z/zMark.hpp"
#undef private
#include "Base/Globals.h"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "gc_heap_fixture.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkContext.hpp"
#include "gc_unittest.hpp"
#include "zunittest.hpp"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/RefField.inline.h"

#if defined(MRT_PARTIAL_ARRAY_FORCED_INTERNALS)
#undef MRT_TESTABLE_INTERNALS
#endif

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

#if defined(MRT_TESTABLE_INTERNALS)
namespace MapleRuntime {

struct PartialArrayTestAccess {
    static void StartFieldMark(Heap& collector)
    {
        auto& old = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
        if (old.Workers() == nullptr) old.InitializeWorkers(1);
        Heap::GetHeap().old().Mark().BindWorkers(Heap::GetHeap().old().Workers());
        Heap::GetHeap().old().Mark().Start();
        old.set_phase(ZGenerationPhase::Mark);
    }

    static void ReadPublished(Heap& collector, WorkStack& result)
    {
        auto& domain = *Heap::GetHeap().old().MarkPtr();
        for (size_t stripe = 0; stripe < domain.Stripes().NStripes(); ++stripe) {
            if (auto* stack = domain.Stacks().StealLocal(domain.Stripes(), domain.Stripes().At(stripe))) {
                while (!stack->IsEmpty()) result.push_back(stack->Pop());
                MarkStripeStack::Destroy(stack);
            }
        }
    }

    static void StoreTarget(const Heap& collector, RefField<>& field, BaseObject* target)
    {
        const RefField<> coloured = ZBarrier::GetAndTryTagRefField(target);
        field.StoreColoured(coloured.GetFieldValue());
    }
};

} // namespace MapleRuntime
#endif

namespace {

using Slot = std::uintptr_t;

void MarkRange(std::set<size_t>& out, size_t begin, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        out.insert(begin + i);
    }
}

std::set<size_t> OffSet(size_t length)
{
    std::set<size_t> s;
    MarkRange(s, 0, length);
    return s;
}

std::set<size_t> OnSet(GcHeapFixture& fx, Slot* addr, size_t length)
{
    WorkerFixture worker;
    auto& domain = Heap::GetHeap().old().Mark();
    domain.PrepareWork(1);
    auto* fields = reinterpret_cast<RefField<>*>(addr);
    std::vector<BaseObject*> objects;
    for (size_t i = 0; i < length; ++i) {
        auto* object = fx.PlaceObject(fx.region0()->GetRegionStart() + i * 64);
        HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE).StoreColoured(zpointer::null);
        objects.push_back(object);
        fields[i].StoreColoured(StoreGoodPointer(object));
    }
    fx.region0()->SetRegionAllocPtr(fx.region0()->GetRegionStart() + (length + 1) * 64);
    RestoreMarkFlips restore;
    ZGlobalsPointers::flip_old_mark_start();
    restore.old = true;
    const auto previous = Heap::GetHeap().old().phase();
    Heap::GetHeap().old().set_phase(ZGenerationPhase::Mark);
    MarkContext context(1, 0, domain.Stripes(), domain.Stacks());
    domain.follow_array_elements(context, reinterpret_cast<MAddress>(addr), length, false);
    (void)domain.FollowWork(context, 0, true);
    context.Cache().Flush();
    Heap::GetHeap().old().set_phase(previous);
    std::set<size_t> observed;
    for (size_t i = 0; i < length; ++i) {
        if (fx.region0()->is_object_strongly_live(from_object(objects[i]))) observed.insert(i);
    }
    std::fprintf(stderr, "ARRAY_COVERAGE entries=%zu marked=%zu live=%u\n",
                 length, observed.size(), fx.region0()->live_objects());
    GC_EXPECT_EQ(fx.region0()->live_objects(), length);
    return observed;
}

std::set<size_t> ExpectSame(GcHeapFixture& fx, Slot* addr, size_t length)
{
    const std::set<size_t> off = OffSet(length);
    const std::set<size_t> on = OnSet(fx, addr, length);
    GC_EXPECT_TRUE(off == on);
    GC_EXPECT_EQ(on.size(), length);
    return on;
}

// Partial-array entries carry ZAddress::offset(chunk) (zMark.cpp:177-183), so
// the slot buffer lives in the heap address domain like every page.
struct SlotBuf {
    Slot* slots = nullptr;

    SlotBuf(size_t n, MAddress base)
    {
        slots = reinterpret_cast<Slot*>(AlignUp(base, MarkPartialArray::MIN_SIZE));
        for (size_t i = 0; i < n; ++i) {
            slots[i] = i + 1;
        }
    }

};


} // namespace

GC_TEST(PartialArray, EncodeDecodeRoundtrip)
{
    GcHeapFixture fx;
    SlotBuf buf(MarkPartialArray::MIN_LENGTH, fx.heapStart + 2 * ZGranuleSize);
    MarkStackEntry entry = MarkPartialArray::Encode(buf.slots, MarkPartialArray::MIN_LENGTH);
    GC_EXPECT_TRUE(entry.partial_array());
    // zMarkStackEntry.hpp:82-83: 32-bit page offset + 30-bit length.
    GC_EXPECT_EQ(entry.partial_array_offset(),
                 untype(ZAddress::offset(to_zaddress(reinterpret_cast<MAddress>(buf.slots)))) >>
                     MarkPartialArray::MIN_SIZE_SHIFT);
    MAddress start = 0;
    size_t length = 0;
    MarkPartialArray::Decode(entry, start, length);
    GC_EXPECT_EQ(start, reinterpret_cast<MAddress>(buf.slots));
    GC_EXPECT_EQ(length, MarkPartialArray::MIN_LENGTH);
}

GC_TEST(PartialArray, PageOffsetChunkRoundtrips)
{
    GcHeapFixture fx;
    SlotBuf buf(MarkPartialArray::MIN_LENGTH * 4, fx.heapStart + 2 * ZGranuleSize);
    constexpr size_t offsets[] = { 1, 8, 1776 };
    for (size_t offset : offsets) {
        const MAddress arrayStart = reinterpret_cast<MAddress>(buf.slots) + offset;
        const MAddress chunkStart = AlignUp(arrayStart + sizeof(Slot),
                                            static_cast<MAddress>(MarkPartialArray::MIN_SIZE));
        const MarkStackEntry entry = MarkPartialArray::Encode(
            reinterpret_cast<const void*>(chunkStart), MarkPartialArray::MIN_LENGTH);
        MAddress decoded = 0;
        size_t decodedLength = 0;
        MarkPartialArray::Decode(entry, decoded, decodedLength);
        GC_EXPECT_EQ(decoded, chunkStart);
        GC_EXPECT_EQ(decodedLength, MarkPartialArray::MIN_LENGTH);
    }
}

#ifdef MRT_TESTABLE_INTERNALS
// Product ZMark::MarkAndFollow consumes the encoded heap offset through its
// partial-array branch (ZGC zMark.cpp:393-401); the sole non-null slot is reached.
GC_OTHER_VM_TEST(PartialArray, ProductPushFollowRoundtrips)
{
    GcHeapFixture fx;
    Heap::OnHeapCreated(fx.heapStart);
    Heap::OnHeapExtended(fx.heapStart + GcHeapFixture::kUnits * ZGranuleSize);
    SlotBuf buf(MarkPartialArray::MIN_LENGTH, fx.heapStart + 2 * ZGranuleSize);
    Heap& collector = Heap::GetHeap();
    WorkStack workStack;
    RefField<>* const chunk = reinterpret_cast<RefField<>*>(buf.slots);
    for (size_t i = 0; i < MarkPartialArray::MIN_LENGTH; ++i) {
        chunk[i].StoreColoured(zpointer::null);
    }
    PartialArrayTestAccess::StoreTarget(collector, chunk[0], fx.obj0);

    const MarkStackEntry partial = MarkPartialArray::Encode(chunk, MarkPartialArray::MIN_LENGTH);
    GC_EXPECT_TRUE(partial.partial_array());

    // #607 fields publish into the generation mark domain, as ZMark's
    // barrier does; the old caller-owned staging stack is not that consumer.
    PartialArrayTestAccess::StartFieldMark(collector);
    ZGlobalsPointers::flip_old_mark_start();
    auto& domain = *Heap::GetHeap().old().MarkPtr();
    MarkContext context(1, 0, domain.Stripes(), domain.Stacks());
    domain.MarkAndFollow(context, partial);
    PartialArrayTestAccess::ReadPublished(collector, workStack);
    GC_EXPECT_FALSE(workStack.empty());
    const MarkStackEntry reached = workStack.back();
    workStack.pop_back();
    GC_EXPECT_FALSE(reached.partial_array());
    GC_EXPECT_TRUE(to_object(ZOffset::address(to_zoffset(reached.object_address()))) == fx.obj0);
}
#endif // MRT_TESTABLE_INTERNALS

GC_TEST(PartialArray, EmptyAndSingle)
{
    GcHeapFixture fx;
    SlotBuf buf(8, fx.heapStart + 2 * ZGranuleSize);
    ExpectSame(fx, buf.slots, 0);
    ExpectSame(fx, buf.slots, 1);
}

GC_TEST(PartialArray, ThresholdExact)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH;
    SlotBuf buf(n, fx.heapStart + 2 * ZGranuleSize);
    ExpectSame(fx, buf.slots, n);
}

GC_TEST(PartialArray, ThresholdMinusOne)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH - 1;
    SlotBuf buf(n, fx.heapStart + 2 * ZGranuleSize);
    ExpectSame(fx, buf.slots, n);
}

GC_TEST(PartialArray, ThresholdPlusOne)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH + 1;
    SlotBuf buf(n, fx.heapStart + 2 * ZGranuleSize);
    ExpectSame(fx, buf.slots, n);
}

GC_TEST(PartialArray, MultiChunk)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH * 8 + 17;
    SlotBuf buf(n, fx.heapStart + 2 * ZGranuleSize);
    ExpectSame(fx, buf.slots, n);
}

GC_OTHER_VM_TEST(PartialArray, BoundaryRefs)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH * 3;
    SlotBuf buf(n, fx.heapStart + 2 * ZGranuleSize);
    const std::set<size_t> on = ExpectSame(fx, buf.slots, n);
    GC_EXPECT_TRUE(on.count(0) == 1);
    GC_EXPECT_TRUE(on.count(MarkPartialArray::MIN_LENGTH - 1) == 1);
    GC_EXPECT_TRUE(on.count(MarkPartialArray::MIN_LENGTH) == 1);
    GC_EXPECT_TRUE(on.count(n - 1) == 1);
}

GC_TEST(PartialArray, UnalignedStart)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH * 4 + 3;
    SlotBuf buf(n + 16, fx.heapStart + 2 * ZGranuleSize);
    ExpectSame(fx, buf.slots + 3, n);
}

// ZGC zMark.cpp:185-196, 471-489. Stop after the first real drain entry
// using the existing abort state, then inspect the product-owned continuations.
GC_TEST(MarkConsumer1328, CrossStripePartialStaysLocalAndOverflowed)
{
    GcHeapFixture fx;
    WorkerFixture worker;
    ZMark domain(4, MarkingStacks::MarkingGeneration::MAJOR);
    domain.PrepareWork(4);
    auto& stripes = domain.Stripes();
    MarkThreadLocalStacks stacks(4);
    MarkContext context(4, 0, stripes, stacks);
    MAddress start = AlignUp(fx.heapStart + 2 * ZGranuleSize, MarkPartialArray::MIN_SIZE);
    while (stripes.StripeForAddress(start) == context.Stripe()) start += MarkPartialArray::MIN_SIZE;
    constexpr size_t length = 3 * MarkPartialArray::MIN_LENGTH + 17;
    std::memset(reinterpret_cast<void*>(start), 0, length * sizeof(MAddress));
    auto* target = stripes.StripeForAddress(start + MarkPartialArray::MIN_SIZE);
    GC_EXPECT_TRUE(target != context.Stripe());
    auto* full = MarkStripeStack::Create(true);
    const size_t capacity = full->Capacity();
    const auto empty = MarkPartialArray::Encode(reinterpret_cast<void*>(start), 0);
    while (!full->IsFull()) full->Push(empty);
    stacks.Install(stripes, target, full);
    stacks.Push(stripes, context.Stripe(), MarkPartialArray::Encode(reinterpret_cast<void*>(start), length), false);
    ZAbort::abort();
    const bool drained = domain.Drain(context, 0);
    ZAbort::reset();
    auto* local = stacks.StealLocal(stripes, target);
    const size_t localCount = local == nullptr ? 0 : local->Size();
    const size_t published = target->published.Length();
    const size_t overflowed = target->overflowed.Length();
    std::fprintf(stderr, "PARTIAL1328_TARGET entries=1 capacity=%zu local=%zu published=%zu overflowed=%zu wake=%zu\n",
                 capacity, localCount, published, overflowed, domain.Terminate().awakening);
    if (local != nullptr) MarkStripeStack::Destroy(local);
    while (auto* stack = target->StealStack(domain.Smr(), 0)) MarkStripeStack::Destroy(stack);
    // One target verdict includes placement and wake state, before diagnostics.
    GC_EXPECT_TRUE(localCount > 0 && published == 0 && overflowed == 1 && domain.Terminate().awakening == 0);
    GC_EXPECT_FALSE(drained);
}

GC_TEST(MarkConsumer1328, PartialDoesNotWakeWithoutFullStack)
{
    GcHeapFixture fx;
    WorkerFixture worker;
    ZMark domain(4, MarkingStacks::MarkingGeneration::MAJOR);
    domain.PrepareWork(4);
    auto& stripes = domain.Stripes();
    MarkThreadLocalStacks stacks(4);
    MarkContext context(4, 0, stripes, stacks);
    MAddress start = AlignUp(fx.heapStart + 2 * ZGranuleSize, MarkPartialArray::MIN_SIZE);
    while (stripes.StripeForAddress(start) == context.Stripe()) start += MarkPartialArray::MIN_SIZE;
    constexpr size_t length = 3 * MarkPartialArray::MIN_LENGTH + 17;
    std::memset(reinterpret_cast<void*>(start), 0, length * sizeof(MAddress));
    domain.Terminate().Leave();
    domain.MarkAndFollow(context, MarkPartialArray::Encode(reinterpret_cast<void*>(start), length));
    auto* target = stripes.StripeForAddress(start + MarkPartialArray::MIN_SIZE);
    auto* local = stacks.StealLocal(stripes, target);
    const size_t count = local == nullptr ? 0 : local->Size();
    const size_t wake = domain.Terminate().awakening;
    if (local != nullptr) MarkStripeStack::Destroy(local);
    std::fprintf(stderr, "PARTIAL1328_WAKE entries=1 local=%zu wake=%zu\n", count, wake);
    GC_EXPECT_TRUE(count > 0 && target->published.IsEmpty() && wake == 0);
    // Positive control: the same terminate state responds to a real wake.
    domain.Terminate().Wake();
    GC_EXPECT_EQ(domain.Terminate().awakening, 1u);
}

GC_TEST(MarkConsumer1328, YoungDrainRejectsAllocatingOldPage)
{
    const char* childFlag = "GC_UNIT_1328_INVALID_PAGE";
    if (std::getenv(childFlag) != nullptr) {
        signal(SIGABRT, SIG_DFL);
        GcHeapFixture fx;
        WorkerFixture worker;
        // ZGC zPage.inline.hpp:184-186: allocating, irrespective of generation.
        fx.region0()->reset(PageAge::old);
        ZMark domain(1, MarkingStacks::MarkingGeneration::YOUNG);
        domain.PrepareWork(1);
        MarkThreadLocalStacks stacks(1);
        MarkContext ctx(1, 0, domain.Stripes(), stacks);
        stacks.Push(domain.Stripes(), ctx.Stripe(),
            MarkStackEntry(untype(ZAddress::offset(from_object(fx.obj0))), true, false, false, false), false);
        std::fprintf(stderr, "INVALID_PAGE1328_INPUT entries=1 old=%d relocatable=%d\n",
                     !fx.region0()->IsYoungRegion(), fx.region0()->IsRelocatable());
        domain.Drain(ctx, 0);
        return;
    }
    GC_EXPECT_EQ(setenv(childFlag, "1", 1), 0);
    try {
        RunInOtherVm("MarkConsumer1328.YoungDrainRejectsAllocatingOldPage",
                     "mark consumer requires a relocatable page");
    } catch (...) {
        unsetenv(childFlag);
        throw;
    }
    unsetenv(childFlag);
    std::fprintf(stderr, "INVALID_PAGE1328_TARGET entries=1 matched_page_assert=1\n");
}
