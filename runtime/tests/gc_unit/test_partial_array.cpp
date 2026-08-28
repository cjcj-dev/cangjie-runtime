// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Partial-array chunking: ON vs OFF must visit the same element set.
// Split transcribed from WCollector.cpp FollowArrayElements / FollowArrayElementsLarge
// (zMark.cpp:208-263). Criterion is set equality, not "did not crash".

#include <cstdint>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <set>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#if !defined(MRT_TESTABLE_INTERNALS)
// WCollector's friend declaration is test-gated.  Keep this TU buildable in
// the default standalone configuration by opening that gate while the header
// is parsed, without enabling the test-only product arm below.
#define MRT_TESTABLE_INTERNALS 1
#define MRT_PARTIAL_ARRAY_FORCED_INTERNALS 1
#endif

#include "Base/Globals.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/MarkPartialArray.h"
#include "gc_heap_fixture.hpp"
#include "Heap/WCollector/WCollector.h"
#include "gc_unittest.hpp"
#include "ObjectModel/MArray.inline.h"

#if defined(MRT_PARTIAL_ARRAY_FORCED_INTERNALS)
#undef MRT_TESTABLE_INTERNALS
#endif

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

struct PartialArrayTestAccess {
#if defined(MRT_TESTABLE_INTERNALS) || defined(MRT_PARTIAL_ARRAY_FORCED_INTERNALS)
    static void Push(const WCollector& collector, RefField<>* addr, size_t length,
                     TracingCollector::WorkStack& workStack)
    {
        collector.PushPartialArray(addr, length, workStack);
    }
#endif

    static void StoreTarget(const WCollector& collector, RefField<>& field, BaseObject* target)
    {
        const RefField<> coloured = collector.GetAndTryTagRefField(target);
        field.StoreColoured(coloured.GetFieldValue());
    }
};

} // namespace MapleRuntime

namespace {

using Slot = std::uintptr_t;

// A real MArray carrier is required here: the product mark visitor only enters
// FollowArrayElements after TraceObjectRefFields recognizes a raw reference
// array (Mark.cpp:472-490).  This mirrors the small type-info fixture used by
// the existing array tests, but keeps the storage local to this test TU.
struct ReferenceArrayTypeInfos {
    ReferenceArrayTypeInfos()
    {
        std::memset(componentStorage, 0, sizeof(componentStorage));
        component = reinterpret_cast<TypeInfo*>(componentStorage);
        component->SetType(TypeKind::TYPE_KIND_CLASS);
        component->SetInstanceSize(sizeof(void*));

        std::memset(arrayStorage, 0, sizeof(arrayStorage));
        array = reinterpret_cast<TypeInfo*>(arrayStorage);
        array->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        array->SetFlagHasRefField();
        array->SetComponentTypeInfo(component);

        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(this), sizeof(*this));
    }

    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)];
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)];
    TypeInfo* component = nullptr;
    TypeInfo* array = nullptr;
};

ReferenceArrayTypeInfos& GetReferenceArrayTypeInfos()
{
    static ReferenceArrayTypeInfos infos;
    return infos;
}

struct ProductArrayBuf {
    size_t bytes = 0;
    void* mem = nullptr;
    MArray* array = nullptr;
    RefField<>* fields = nullptr;

    ProductArrayBuf(size_t n, size_t contentPhase, TypeInfo* arrayType)
    {
        GC_EXPECT_TRUE(contentPhase < MarkPartialArray::MIN_SIZE);
        GC_EXPECT_EQ(contentPhase % sizeof(Slot), static_cast<size_t>(0));
        bytes = AlignUp(MArray::GetContentOffset() + (n + 8) * sizeof(Slot) +
                            MarkPartialArray::MIN_SIZE,
                        MarkPartialArray::MIN_SIZE);
        int mapFlags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__x86_64__)
        // Keep the absolute-address fault predicate's 32-bit page-offset
        // guard in range; the production predicate remains heap-relative.
        mapFlags |= MAP_32BIT;
#endif
        mem = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, mapFlags, -1, 0);
        if (mem == MAP_FAILED) {
            std::abort();
        }
        const MAddress raw = reinterpret_cast<MAddress>(mem);
        const MAddress alignedContent =
            AlignUp(raw + MArray::GetContentOffset(), static_cast<MAddress>(MarkPartialArray::MIN_SIZE)) +
            contentPhase;
        array = reinterpret_cast<MArray*>(alignedContent - MArray::GetContentOffset());
        array->SetClassInfo(arrayType);
        array->SetLength(static_cast<MIndex>(n));
        fields = &HeapSlotAt<>(alignedContent);
    }

    ~ProductArrayBuf()
    {
        if (mem != nullptr && mem != MAP_FAILED) {
            munmap(mem, bytes);
        }
    }
};

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

    // Keep the backing mapping page aligned, then place the array at an
    // explicit byte phase within that page.  The old fixture relied on the
    // relative order of two independent mmap calls, so the same test could
    // select either the Encodable or the inline fallback path.
    explicit SlotBuf(size_t n, size_t pageOffset = 0)
    {
        GC_EXPECT_TRUE(pageOffset < MarkPartialArray::MIN_SIZE);
        GC_EXPECT_EQ(pageOffset % sizeof(Slot), static_cast<size_t>(0));
        bytes = AlignUp((n + 8) * sizeof(Slot) + MarkPartialArray::MIN_SIZE + pageOffset,
                        MarkPartialArray::MIN_SIZE);
        mem = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) {
            std::abort();
        }
        auto raw = reinterpret_cast<uintptr_t>(mem);
        slots = reinterpret_cast<Slot*>(AlignUp(raw, MarkPartialArray::MIN_SIZE) + pageOffset);
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

#if defined(MRT_TESTABLE_INTERNALS) && !defined(MRT_PARTIAL_ARRAY_FORCED_INTERNALS)
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

void CheckBoundaryRefs(Slot* slots, size_t length)
{
    const std::set<size_t> on = ExpectSame(slots, length);
    GC_EXPECT_TRUE(on.count(0) == 1);
    GC_EXPECT_TRUE(on.count(MarkPartialArray::MIN_LENGTH - 1) == 1);
    GC_EXPECT_TRUE(on.count(MarkPartialArray::MIN_LENGTH) == 1);
    GC_EXPECT_TRUE(on.count(length - 1) == 1);
}

void CheckBoundaryRefsProduct(size_t n, size_t contentPhase, bool misalignedBase)
{
    GcHeapFixture fx;
    ReferenceArrayTypeInfos& infos = GetReferenceArrayTypeInfos();
    ProductArrayBuf buf(n, contentPhase, infos.array);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    TracingCollector::WorkStack workStack;

    const MAddress savedStart = Heap::GetHeapStartAddress();
    const MAddress savedEnd = Heap::heapCurrentEnd;
    const MAddress lowAddress = std::min(reinterpret_cast<MAddress>(buf.fields),
                                         reinterpret_cast<MAddress>(fx.obj0));
    const MAddress alignedBase = AlignDown(lowAddress, static_cast<MAddress>(MarkPartialArray::MIN_SIZE));
    const MAddress heapBase = alignedBase + (misalignedBase ? 1 : 0);
    const MAddress highAddress = std::max(reinterpret_cast<MAddress>(buf.mem) + buf.bytes,
                                          savedEnd);
#if defined(MRT_TESTABLE_INTERNALS)
    if (misalignedBase) {
        Heap::SetHeapStartForTesting(heapBase);
    } else {
        Heap::OnHeapCreated(heapBase);
    }
#else
    Heap::OnHeapCreated(heapBase);
#endif
    Heap::OnHeapExtended(highAddress);

    if (misalignedBase) {
        // Positive/negative predicate witness for the separate old-predicate
        // control arm.  The actual traversal below still enters through the
        // product MArray visitor and is the wiring proof.
        GC_EXPECT_FALSE(MarkPartialArray::Encodable(
            reinterpret_cast<const void*>(AlignDown(reinterpret_cast<MAddress>(buf.fields),
                                                     static_cast<MAddress>(MarkPartialArray::MIN_SIZE))),
            1));
    }

    // Colour each slot through the same helper as the production barrier.  A
    // complete drain of the product work stack then gives one ordinary entry
    // per array element; partial entries are fed back through FollowPartialArray
    // so the test covers both FollowArrayElements and PushPartialArray.
    for (size_t i = 0; i < n; ++i) {
        // Alternate two distinct managed objects.  A stale/shifted decode can
        // no longer accidentally read the same repeated word and look valid.
        PartialArrayTestAccess::StoreTarget(collector, buf.fields[i], (i & 1) == 0 ? fx.obj0 : fx.obj1);
    }
    collector.TraceObjectRefFields(reinterpret_cast<BaseObject*>(buf.array), workStack);

    bool sawPartial = false;
    std::vector<MarkStackEntry> pending;
    pending.reserve(workStack.size());
    while (!workStack.empty()) {
        const MarkStackEntry entry = workStack.back();
        workStack.pop_back();
        sawPartial = sawPartial || MarkPartialArray::IsPartialArrayEntry(entry);
        pending.push_back(entry);
    }
    // MarkStack intentionally exposes only push/pop/drain operations; restore
    // the entries after the inspection so the product consumer below drains
    // the exact same work set.
    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
        workStack.push_back(*it);
    }
    // The relative predicate must inline the misaligned-base arm, while both
    // legal phases must enqueue at least one partial chunk.  This assertion is
    // deliberately before draining so a disconnected product call cannot hide
    // behind a coincidentally valid repeated slot value.
    GC_EXPECT_EQ(sawPartial, !misalignedBase);

    size_t visited = 0;
    while (!workStack.empty()) {
        const MarkStackEntry entry = workStack.back();
        workStack.pop_back();
        if (MarkPartialArray::IsPartialArrayEntry(entry)) {
            collector.FollowPartialArray(entry, workStack);
        } else {
            GC_EXPECT_TRUE(entry.object() == fx.obj0 || entry.object() == fx.obj1);
            ++visited;
        }
    }
    GC_EXPECT_EQ(visited, n);

#if defined(MRT_TESTABLE_INTERNALS)
    if (misalignedBase) {
        Heap::SetHeapStartForTesting(savedStart);
    } else {
        Heap::OnHeapCreated(savedStart);
    }
#else
    Heap::OnHeapCreated(savedStart);
#endif
    Heap::OnHeapExtended(savedEnd);
}

GC_OTHER_VM_TEST(PartialArray, BoundaryRefs)
{
    const size_t n = MarkPartialArray::MIN_LENGTH * 3;
    constexpr size_t kRepeats = 20;
    constexpr size_t kPageAligned = 0;
    std::fprintf(stderr, "BOUNDARY_REFS_ARM layout=page-aligned N=%zu\n", kRepeats);
    for (size_t i = 0; i < kRepeats; ++i) {
        CheckBoundaryRefsProduct(n, kPageAligned, false);
    }
}

GC_OTHER_VM_TEST(PartialArray, BoundaryRefsPlusSlot)
{
    const size_t n = MarkPartialArray::MIN_LENGTH * 3;
    constexpr size_t kRepeats = 20;
    constexpr size_t kAfterHeader = sizeof(Slot);
    std::fprintf(stderr, "BOUNDARY_REFS_ARM layout=plus-%zu-byte N=%zu\n", kAfterHeader, kRepeats);
    for (size_t i = 0; i < kRepeats; ++i) {
        CheckBoundaryRefsProduct(n, kAfterHeader, false);
    }
}

#ifdef MRT_TESTABLE_INTERNALS
GC_TEST(PartialArray, BoundaryRefsMisalignedBase)
{
    // Deliberately put the heap base one byte into the page while keeping the
    // array page aligned.  The fixed relative Encodable predicate must take
    // the inline fallback and preserve the full element set.  A regression to
    // the old absolute-alignment predicate instead enters Encode/Decode for
    // the middle chunk and this exact arm turns red.
    const size_t n = MarkPartialArray::MIN_LENGTH * 3;
    constexpr size_t kRepeats = 20;
    std::fprintf(stderr, "BOUNDARY_REFS_ARM layout=misaligned-base-plus-1 N=%zu\n", kRepeats);
    for (size_t i = 0; i < kRepeats; ++i) {
        CheckBoundaryRefsProduct(n, 0, true);
    }
}
#endif // MRT_TESTABLE_INTERNALS

GC_TEST(PartialArray, UnalignedStart)
{
    GcHeapFixture fx;
    const size_t n = MarkPartialArray::MIN_LENGTH * 4 + 3;
    SlotBuf buf(n + 16);
    ExpectSame(buf.slots + 3, n);
}
