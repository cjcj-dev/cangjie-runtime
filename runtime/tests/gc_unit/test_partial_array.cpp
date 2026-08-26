// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Partial-array chunking: ON vs OFF must visit the same element set.
// Split transcribed from WCollector.cpp FollowArrayElements / FollowArrayElementsLarge
// (zMark.cpp:208-263). Criterion is set equality, not "did not crash".

#include <cstdint>
#include <csignal>
#include <cstring>
#include <set>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "Base/Globals.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/MarkPartialArray.h"
#include "gc_heap_fixture.hpp"
#include "Heap/WCollector/WCollector.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

#if defined(MRT_TESTABLE_INTERNALS)
namespace MapleRuntime {

struct PartialArrayTestAccess {
    static void Push(const WCollector& collector, RefField<>* addr, size_t length,
                     TracingCollector::WorkStack& workStack)
    {
        collector.PushPartialArray(addr, length, workStack);
    }

    static void StoreTarget(const WCollector& collector, RefField<>& field, BaseObject* target)
    {
        const RefField<> coloured = collector.GetAndTryTagRefField(target);
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

void FollowSmall(std::set<size_t>& out, size_t begin, size_t length)
{
    MarkRange(out, begin, length);
}

void FollowElements(std::set<size_t>& out, Slot* addr, size_t length, Slot* base);

void FollowLarge(std::set<size_t>& out, Slot* addr, size_t length, Slot* base)
{
    Slot* const start = addr;
    Slot* const end = start + length;
    Slot* const middleStart = AlignUp(start + 1, MarkPartialArray::MIN_SIZE);
    const size_t middleLength =
        AlignDown(static_cast<size_t>(end - middleStart), MarkPartialArray::MIN_LENGTH);
    Slot* const middleEnd = middleStart + middleLength;

    std::vector<std::pair<Slot*, size_t>> pushed;
    if (end > middleEnd) {
        pushed.push_back({ middleEnd, static_cast<size_t>(end - middleEnd) });
    }
    Slot* partialAddr = middleEnd;
    while (partialAddr > middleStart) {
        const size_t parts = 2;
        const size_t partialLength = AlignUp(static_cast<size_t>(partialAddr - middleStart) / parts,
                                             MarkPartialArray::MIN_LENGTH);
        partialAddr -= partialLength;
        pushed.push_back({ partialAddr, partialLength });
    }

    FollowSmall(out, static_cast<size_t>(start - base), static_cast<size_t>(middleStart - start));
    for (auto& chunk : pushed) {
        if (MarkPartialArray::Encodable(chunk.first, chunk.second)) {
            MarkStackEntry entry = MarkPartialArray::Encode(chunk.first, chunk.second);
            GC_EXPECT_TRUE(MarkPartialArray::IsPartialArrayEntry(entry));
            MAddress decoded = 0;
            size_t decodedLen = 0;
            MarkPartialArray::Decode(entry, decoded, decodedLen);
            GC_EXPECT_EQ(decoded, reinterpret_cast<MAddress>(chunk.first));
            GC_EXPECT_EQ(decodedLen, chunk.second);
        }
        FollowElements(out, chunk.first, chunk.second, base);
    }
}

void FollowElements(std::set<size_t>& out, Slot* addr, size_t length, Slot* base)
{
    if (length <= MarkPartialArray::MIN_LENGTH) {
        FollowSmall(out, static_cast<size_t>(addr - base), length);
        return;
    }
    if (length > MarkPartialArray::MAX_LENGTH ||
        !MarkPartialArray::Encodable(
            reinterpret_cast<const void*>(
                AlignDown(reinterpret_cast<MAddress>(addr + length),
                          static_cast<MAddress>(MarkPartialArray::MIN_SIZE))),
            1)) {
        FollowSmall(out, static_cast<size_t>(addr - base), length);
        return;
    }
    FollowLarge(out, addr, length, base);
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
    FollowElements(s, addr, length, addr);
    return s;
}

std::set<size_t> ExpectSame(Slot* addr, size_t length)
{
    const MAddress savedStart = Heap::GetHeapStartAddress();
    const MAddress savedEnd = Heap::heapCurrentEnd;
    const MAddress lo = AlignDown(reinterpret_cast<MAddress>(addr),
                                  static_cast<MAddress>(MarkPartialArray::MIN_SIZE));
    Heap::OnHeapCreated(lo);
    Heap::OnHeapExtended(lo + (length + 16) * sizeof(Slot) + MarkPartialArray::MIN_SIZE);
    const std::set<size_t> off = OffSet(length);
    const std::set<size_t> on = OnSet(addr, length);
    Heap::OnHeapCreated(savedStart);
    Heap::OnHeapExtended(savedEnd);
    GC_EXPECT_EQ(off.size(), on.size());
    GC_EXPECT_TRUE(off == on);
    GC_EXPECT_EQ(off.size(), length);
    return on;
}

struct SlotBuf {
    size_t bytes = 0;
    void* mem = nullptr;
    Slot* slots = nullptr;

    explicit SlotBuf(size_t n)
    {
        bytes = AlignUp((n + 8) * sizeof(Slot) + MarkPartialArray::MIN_SIZE, MarkPartialArray::MIN_SIZE);
        mem = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) {
            std::abort();
        }
        auto raw = reinterpret_cast<uintptr_t>(mem);
        slots = reinterpret_cast<Slot*>(AlignUp(raw, MarkPartialArray::MIN_SIZE));
        for (size_t i = 0; i < n; ++i) {
            slots[i] = i + 1;
        }
    }

    ~SlotBuf()
    {
        if (mem != nullptr && mem != MAP_FAILED) {
            munmap(mem, bytes);
        }
    }
};

#if defined(MRT_TESTABLE_INTERNALS)
class HeapBaseOverride {
public:
    explicit HeapBaseOverride(MAddress base)
        : savedStart(Heap::GetHeapStartAddress()), savedEnd(Heap::heapCurrentEnd)
    {
        Heap::SetHeapStartForTesting(base);
    }

    ~HeapBaseOverride()
    {
        Heap::SetHeapStartForTesting(savedStart);
        Heap::OnHeapExtended(savedEnd);
    }

private:
    MAddress savedStart;
    MAddress savedEnd;
};

struct LowAddressSlots {
    static constexpr size_t PAGE_COUNT = 5;

    void* mem = nullptr;

    LowAddressSlots()
    {
        // Keep the arbitrary-base test inside MarkStackEntry's 32-bit page-offset
        // domain. MAP_FIXED_NOREPLACE preserves unrelated mappings if a candidate
        // is already occupied.
        constexpr MAddress candidates[] = {
            static_cast<MAddress>(0x100000000ULL),
            static_cast<MAddress>(0x200000000ULL),
            static_cast<MAddress>(0x300000000ULL),
        };
        for (MAddress candidate : candidates) {
            mem = mmap(reinterpret_cast<void*>(candidate), PAGE_COUNT * MarkPartialArray::MIN_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (mem != MAP_FAILED) {
                break;
            }
        }
        if (mem == MAP_FAILED) {
            std::abort();
        }
        std::memset(mem, 0, PAGE_COUNT * MarkPartialArray::MIN_SIZE);
    }

    ~LowAddressSlots()
    {
        if (mem != nullptr && mem != MAP_FAILED) {
            munmap(mem, PAGE_COUNT * MarkPartialArray::MIN_SIZE);
        }
    }

    RefField<>* AbsoluteAlignedChunk() const
    {
        return reinterpret_cast<RefField<>*>(mem);
    }

    RefField<>* RelativeAlignedChunk() const
    {
        const MAddress chunk = reinterpret_cast<MAddress>(mem) + 2 * MarkPartialArray::MIN_SIZE + 1;
        return reinterpret_cast<RefField<>*>(chunk);
    }
};
#endif

} // namespace

GC_TEST(PartialArray, EncodeDecodeRoundtrip)
{
    GcHeapFixture fx;
    SlotBuf buf(MarkPartialArray::MIN_LENGTH);
    const MAddress savedStart = Heap::GetHeapStartAddress();
    const MAddress savedEnd = Heap::heapCurrentEnd;
    Heap::OnHeapCreated(reinterpret_cast<MAddress>(buf.slots));
    Heap::OnHeapExtended(reinterpret_cast<MAddress>(buf.slots) + buf.bytes);
    GC_EXPECT_TRUE(MarkPartialArray::Encodable(buf.slots, MarkPartialArray::MIN_LENGTH));
    MarkStackEntry entry = MarkPartialArray::Encode(buf.slots, MarkPartialArray::MIN_LENGTH);
    GC_EXPECT_TRUE(MarkPartialArray::IsPartialArrayEntry(entry));
    MAddress start = 0;
    size_t length = 0;
    MarkPartialArray::Decode(entry, start, length);
    GC_EXPECT_EQ(start, reinterpret_cast<MAddress>(buf.slots));
    GC_EXPECT_EQ(length, MarkPartialArray::MIN_LENGTH);
    Heap::OnHeapCreated(savedStart);
    Heap::OnHeapExtended(savedEnd);
}

GC_TEST(PartialArray, HeapStartRequiresMinSizeAlignment)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        Heap::OnHeapCreated(MarkPartialArray::MIN_SIZE + 1);
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
}

GC_TEST(PartialArray, PageOffsetChunkRoundtrips)
{
    GcHeapFixture fx;
    SlotBuf buf(MarkPartialArray::MIN_LENGTH * 4);
    const MAddress savedStart = Heap::GetHeapStartAddress();
    const MAddress savedEnd = Heap::heapCurrentEnd;
    Heap::OnHeapCreated(reinterpret_cast<MAddress>(buf.slots));
    Heap::OnHeapExtended(reinterpret_cast<MAddress>(buf.slots) + buf.bytes);
    constexpr size_t offsets[] = { 1, 8, 1776 };
    for (size_t offset : offsets) {
        const MAddress arrayStart = reinterpret_cast<MAddress>(buf.slots) + offset;
        const MAddress chunkStart = AlignUp(arrayStart + sizeof(Slot),
                                            static_cast<MAddress>(MarkPartialArray::MIN_SIZE));
        GC_EXPECT_TRUE(MarkPartialArray::Encodable(reinterpret_cast<const void*>(chunkStart),
                                                   MarkPartialArray::MIN_LENGTH));
        const MarkStackEntry entry = MarkPartialArray::Encode(
            reinterpret_cast<const void*>(chunkStart), MarkPartialArray::MIN_LENGTH);
        MAddress decoded = 0;
        size_t decodedLength = 0;
        MarkPartialArray::Decode(entry, decoded, decodedLength);
        GC_EXPECT_EQ(decoded, chunkStart);
        GC_EXPECT_EQ(decodedLength, MarkPartialArray::MIN_LENGTH);
    }
    Heap::OnHeapCreated(savedStart);
    Heap::OnHeapExtended(savedEnd);
}

#ifdef MRT_TESTABLE_INTERNALS
GC_TEST(PartialArray, EncodableRejectsAbsoluteOnlyAlignment)
{
    // B=4097 and any page-aligned A form the absolute-only counterexample:
    // A is page aligned, but (A-B) has page phase 4095.
    GcHeapFixture fx;
    LowAddressSlots slots;
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    TracingCollector::WorkStack workStack;
    HeapBaseOverride base(static_cast<MAddress>(4097));
    RefField<>* const chunk = slots.AbsoluteAlignedChunk();

    GC_EXPECT_TRUE((reinterpret_cast<MAddress>(chunk) & (MarkPartialArray::MIN_SIZE - 1)) == 0);
    GC_EXPECT_TRUE((Heap::GetHeapStartAddress() & (MarkPartialArray::MIN_SIZE - 1)) != 0);
    PartialArrayTestAccess::Push(collector, chunk, MarkPartialArray::MIN_LENGTH, workStack);
    const bool followedInline = workStack.empty();
    if (!followedInline) {
        // A deliberately disconnected Encodable guard leaves an invalid
        // descriptor here. Remove it before reporting the exact expectation.
        workStack.pop_back();
    }
    GC_EXPECT_TRUE(followedInline);
}

GC_TEST(PartialArray, RelativeBaseRoundtrips)
{
    // B=4097 and A%4096=1 must take the product Push -> Encode handoff.
    // Follow then decodes A and reaches the sole non-null slot at that address.
    GcHeapFixture fx;
    LowAddressSlots slots;
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    TracingCollector::WorkStack workStack;
    HeapBaseOverride base(static_cast<MAddress>(4097));
    RefField<>* const chunk = slots.RelativeAlignedChunk();
    PartialArrayTestAccess::StoreTarget(collector, chunk[0], fx.obj0);

    PartialArrayTestAccess::Push(collector, chunk, MarkPartialArray::MIN_LENGTH, workStack);
    GC_EXPECT_FALSE(workStack.empty());
    const MarkStackEntry partial = workStack.back();
    workStack.pop_back();
    GC_EXPECT_TRUE(MarkPartialArray::IsPartialArrayEntry(partial));

    collector.FollowPartialArray(partial, workStack);
    GC_EXPECT_FALSE(workStack.empty());
    const MarkStackEntry reached = workStack.back();
    workStack.pop_back();
    GC_EXPECT_FALSE(MarkPartialArray::IsPartialArrayEntry(reached));
    GC_EXPECT_TRUE(reached.object() == fx.obj0);
}
#endif // MRT_TESTABLE_INTERNALS

GC_TEST(PartialArray, EmptyAndSingle)
{
    GcHeapFixture fx;
    SlotBuf buf(8);
    ExpectSame(buf.slots, 0);
    ExpectSame(buf.slots, 1);
}

GC_TEST(PartialArray, ThresholdExact)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH;
    SlotBuf buf(n);
    ExpectSame(buf.slots, n);
}

GC_TEST(PartialArray, ThresholdMinusOne)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH - 1;
    SlotBuf buf(n);
    ExpectSame(buf.slots, n);
}

GC_TEST(PartialArray, ThresholdPlusOne)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH + 1;
    SlotBuf buf(n);
    ExpectSame(buf.slots, n);
}

GC_TEST(PartialArray, MultiChunk)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH * 8 + 17;
    SlotBuf buf(n);
    ExpectSame(buf.slots, n);
}

GC_OTHER_VM_TEST(PartialArray, BoundaryRefs)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH * 3;
    SlotBuf buf(n);
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
    SlotBuf buf(n + 16);
    ExpectSame(buf.slots + 3, n);
}
