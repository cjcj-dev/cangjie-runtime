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
            if (auto* stack = domain.Stacks().StealLocal(stripe)) {
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

std::set<size_t> OnSet(Slot* addr, size_t length)
{
    std::set<size_t> s;
    std::vector<MarkStackEntry> pending;
    const MAddress base = reinterpret_cast<MAddress>(addr);
    auto visit = [&](MAddress field) {
        GC_EXPECT_TRUE(field >= base);
        GC_EXPECT_EQ((field - base) % sizeof(Slot), 0u);
        const size_t index = (field - base) / sizeof(Slot);
        GC_EXPECT_TRUE(index < length);
        GC_EXPECT_TRUE(s.insert(index).second);
    };
    auto publish = [&](const MarkStackEntry& entry) { pending.push_back(entry); };
    MarkPartialArray::FollowElements(base, length, false, visit, publish);
    while (!pending.empty()) {
        const MarkStackEntry entry = pending.back();
        pending.pop_back();
        GC_EXPECT_TRUE(entry.partial_array());
        MarkPartialArray::FollowPartialReferences(entry, visit, publish);
    }
    return s;
}

std::set<size_t> ExpectSame(Slot* addr, size_t length)
{
    const std::set<size_t> off = OffSet(length);
    const std::set<size_t> on = OnSet(addr, length);
    GC_EXPECT_EQ(off.size(), on.size());
    GC_EXPECT_TRUE(off == on);
    GC_EXPECT_EQ(off.size(), length);
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
    GC_EXPECT_TRUE(MarkPartialArray::IsPartialArrayEntry(entry));
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
    GC_EXPECT_TRUE(MarkPartialArray::IsPartialArrayEntry(partial));

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
    GC_EXPECT_FALSE(MarkPartialArray::IsPartialArrayEntry(reached));
    GC_EXPECT_TRUE(to_object(ZOffset::address(to_zoffset(reached.object_address()))) == fx.obj0);
}
#endif // MRT_TESTABLE_INTERNALS

GC_TEST(PartialArray, EmptyAndSingle)
{
    GcHeapFixture fx;
    SlotBuf buf(8, fx.heapStart + 2 * ZGranuleSize);
    ExpectSame(buf.slots, 0);
    ExpectSame(buf.slots, 1);
}

GC_TEST(PartialArray, ThresholdExact)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH;
    SlotBuf buf(n, fx.heapStart + 2 * ZGranuleSize);
    ExpectSame(buf.slots, n);
}

GC_TEST(PartialArray, ThresholdMinusOne)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH - 1;
    SlotBuf buf(n, fx.heapStart + 2 * ZGranuleSize);
    ExpectSame(buf.slots, n);
}

GC_TEST(PartialArray, ThresholdPlusOne)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH + 1;
    SlotBuf buf(n, fx.heapStart + 2 * ZGranuleSize);
    ExpectSame(buf.slots, n);
}

GC_TEST(PartialArray, MultiChunk)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH * 8 + 17;
    SlotBuf buf(n, fx.heapStart + 2 * ZGranuleSize);
    ExpectSame(buf.slots, n);
}

GC_OTHER_VM_TEST(PartialArray, BoundaryRefs)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH * 3;
    SlotBuf buf(n, fx.heapStart + 2 * ZGranuleSize);
    const std::set<size_t> on = ExpectSame(buf.slots, n);
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
    ExpectSame(buf.slots + 3, n);
}
