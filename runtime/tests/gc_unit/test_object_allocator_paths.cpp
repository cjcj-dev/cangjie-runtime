// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Cangjie.h"
#include "gc_unittest.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zGlobals.hpp"
#include "TypeInfoManager.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/MObject.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/MArray.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo*, MSize);
}
namespace {
void* AllocateSizedObjects(void*)
{
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)];
    std::memset(storage, 0, sizeof(storage));
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    // Product entry sizes straddle both routing boundaries. Each result is
    // observed through the product page table, not a test-side page model.
    const size_t sizes[] = {256, ZObjectSizeLimitSmall + 8, ZObjectSizeLimitMedium + 8};
    for (size_t i = 0; i < 3; ++i) {
        if (i == 1 && !ZPageSizeMediumEnabled) { continue; }
        const size_t size = sizes[i];
        type->SetInstanceSize(size - TYPEINFO_PTR_SIZE);
        const auto first = reinterpret_cast<uintptr_t>(MCC_NewObject(type, size));
        const auto second = reinterpret_cast<uintptr_t>(MCC_NewObject(type, size));
        if (first == 0 || second == 0) { return reinterpret_cast<void*>(1); }
        const auto* page = Heap::page(first);
        const auto* next = Heap::page(second);
        const bool distinctObjects = first != second;
        const bool correctType = page != nullptr && (i == 0 ? page->IsSmallRegion() :
                                 i == 1 ? (page->type() == ZPageType::medium) : page->IsLargeRegion());
        const bool shared = i == 2 ? page != next : page == next;
        const bool geometry = i != 2 || (page != nullptr && page->size() == AlignUp(size, ZGranuleSize));
        const auto* buffer = AllocBuffer::GetAllocBuffer();
        const bool tlab = i != 0 || (buffer != nullptr && buffer->TLABSize() >= size && buffer->GetRegion() == page);
        std::fprintf(stderr, "OBJECT_ALLOCATOR_TARGET branch=%zu size=%zu page_size=%zu type=%d sharing=%d distinct=%d tlab=%d geometry=%d\n",
                     i, size, page == nullptr ? 0 : page->GetRegionSize(), correctType, shared, distinctObjects, tlab, geometry);
        if (!correctType || !shared || !distinctObjects || !tlab || !geometry) { return reinterpret_cast<void*>(2 + i); }
    }
    return nullptr;
}
}
static void RunAllocatorCase(CJTaskFunc task, bool queuedCollectionAtShutdown = false)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    CJThreadHandle handle = RunCJTask(task, nullptr);
    GC_EXPECT_TRUE(handle != nullptr);
    void* result = nullptr;
    std::fprintf(stderr, "ALLOCATOR_PHASE before_get_task_ret\n");
    GC_EXPECT_EQ(GetTaskRet(handle, &result), E_OK);
    std::fprintf(stderr, "ALLOCATOR_PHASE after_get_task_ret\n");
    ReleaseHandle(handle);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(result), uintptr_t{0});
#if defined(MRT_TESTABLE_INTERNALS)
    std::atomic<bool> queued{false};
    std::atomic<bool> finiCompleted{false};
    bool abortObserved = false;
    std::thread collection;
    if (queuedCollectionAtShutdown) {
        collection = std::thread([&] {
            // ZGC zDriver.cpp:207-220: acquire the driver lock before testing abort.
            // Queue a real collection while holding that same product lock;
            // release it only after Fini has published its stop request.
            ZDriver::lock();
            ZCollectedHeap::heap()->driver_minor()->collect(ZDriverRequest(GC_REASON_ALLOCATION_STALL, 0, 0));
            queued.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!ZAbort::should_abort() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            abortObserved = ZAbort::should_abort();
            std::fprintf(stderr, "MEDIUM_SHUTDOWN_RELEASE abort_observed=%d\n", abortObserved);
            ZDriver::unlock();
            const auto finishDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!finiCompleted.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < finishDeadline) {
                std::this_thread::yield();
            }
            const bool completed = finiCompleted.load(std::memory_order_acquire);
            std::fprintf(stderr, "MEDIUM_SHUTDOWN_COMPLETION_TARGET completed=%d abort_retained=%d\n",
                         completed, ZAbort::should_abort());
            if (!completed) {
                try { GC_EXPECT_TRUE(completed); }
                catch (const std::exception& error) {
                    std::fprintf(stderr, "MEDIUM_SHUTDOWN_COMPLETION_ASSERTION %s\n", error.what());
                    std::_Exit(1);
                }
            }
        });
        while (!queued.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    }
#else
    (void)queuedCollectionAtShutdown;
#endif
    std::fprintf(stderr, "ALLOCATOR_PHASE before_fini\n");
    const auto finiResult = FiniCJRuntime();
#if defined(MRT_TESTABLE_INTERNALS)
    finiCompleted.store(true, std::memory_order_release);
    if (collection.joinable()) { collection.join(); }
    if (queuedCollectionAtShutdown) {
        const bool abortRetained = ZAbort::should_abort();
        std::fprintf(stderr, "MEDIUM_SHUTDOWN_TARGET fini=%d observed=%d retained=%d\n",
                     finiResult, abortObserved, abortRetained);
        GC_EXPECT_TRUE(abortObserved && abortRetained);
    }
#endif
    GC_EXPECT_EQ(finiResult, E_OK);
    std::fprintf(stderr, "ALLOCATOR_PHASE after_fini\n");
}

namespace {
void* AllocateTLABSlices(void*)
{
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)];
    std::memset(storage, 0, sizeof(storage));
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    constexpr size_t bytes = 256;
    type->SetInstanceSize(bytes - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    auto* buffer = AllocBuffer::GetAllocBuffer();
    buffer->ClearRegion();
    const size_t requested = buffer->ComputeTLABSize(bytes, Heap::GetHeap().unsafe_max_tlab_alloc());
    const uintptr_t first = reinterpret_cast<uintptr_t>(MCC_NewObject(type, bytes));
    const size_t actual = buffer->TLABSize();
    buffer->ClearRegion();
    const uintptr_t second = reinterpret_cast<uintptr_t>(MCC_NewObject(type, bytes));
    const bool shared = first != 0 && second == first + requested && Heap::page(first) == Heap::page(second);
    const bool descriptor = actual == requested;
    std::fprintf(stderr, "TLAB_SHARED_TARGET first=%#zx second=%#zx shared=%d requested=%zu actual=%zu descriptor=%d\n",
                 first, second, shared, requested, actual, descriptor);
    return reinterpret_cast<void*>(shared && descriptor ? 0 : 1);
}
void* AllocateManagedFastMedium(void*)
{
    auto& manager = Heap::GetHeap().page_allocator();
    if (!ZPageSizeMediumEnabled) { return reinterpret_cast<void*>(1); }
    ZPage* cached = Heap::alloc_page(ZPageSizeMediumMin, ZPageType::medium, false, false);
    if (cached == nullptr) { return reinterpret_cast<void*>(2); }
    Heap::free_page(cached);
    const size_t capacity = manager.GetCommittedBytes();
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)];
    std::memset(storage, 0, sizeof(storage));
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    const size_t bytes = ZObjectSizeLimitSmall + 8;
    type->SetInstanceSize(bytes - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    const uintptr_t first = reinterpret_cast<uintptr_t>(MCC_NewObject(type, bytes));
    const uintptr_t second = reinterpret_cast<uintptr_t>(MCC_NewObject(type, bytes));
    const ZPage* page = first == 0 ? nullptr : Heap::page(first);
    const size_t actual = page == nullptr ? 0 : page->size();
    const bool valid = page != nullptr && page->type() == ZPageType::medium && actual == ZPageSizeMediumMin &&
                       second == first + AlignUp(bytes, static_cast<size_t>(ZObjectAlignmentMedium)) && Heap::page(second) == page && manager.GetCommittedBytes() == capacity;
    std::fprintf(stderr, "MANAGED_FAST_MEDIUM_TARGET min=%zu actual=%zu capacity_before=%zu capacity_after=%zu stride=%zu alignment=%d valid=%d\n",
                 ZPageSizeMediumMin, actual, capacity, manager.GetCommittedBytes(), second - first, ZObjectAlignmentMedium, valid);
    return reinterpret_cast<void*>(valid ? 0 : 3);
}
void* AllocateMediumNonBlocking(void*)
{
    auto& heap = Heap::GetHeap();
    const size_t bytes = ZObjectSizeLimitSmall + 8;
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)];
    std::memset(storage, 0, sizeof(storage));
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(bytes - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    const uint64_t before = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
    const uintptr_t result = heap.object_allocator().alloc_for_relocation(bytes, PageAge::eden);
    if (result != 0) { reinterpret_cast<BaseObject*>(result)->SetClassInfo(type); }
    const ZPage* page = result == 0 ? nullptr : Heap::page(result);
    const size_t actual = page == nullptr ? 0 : page->size();
    const uint64_t after = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
    const bool valid = page != nullptr && page->type() == ZPageType::medium && actual == ZPageSizeMediumMax && before == after;
    std::fprintf(stderr, "MEDIUM_NONBLOCKING_TARGET actual=%zu expected=%zu before=%llu after=%llu valid=%d\n",
                 actual, ZPageSizeMediumMax, (unsigned long long)before, (unsigned long long)after, valid);
    return reinterpret_cast<void*>(valid ? 0 : 1);
}
void* AllocateMediumBlockingFailure(void*)
{
    auto& heap = Heap::GetHeap();
    const size_t occupiedBytes = heap.GetMaxCapacity() - ZGranuleSize;
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)];
    std::memset(storage, 0, sizeof(storage));
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(occupiedBytes - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    auto* occupied = MCC_NewObject(type, occupiedBytes);
    const U64 root = heap.RegisterExportRoot(occupied);
    Mutator* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    const uint64_t before = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
    // The rooted large object leaves less than a medium page. A blocking
    // allocator must attempt collection before reporting the terminal failure.
    const uintptr_t result = heap.object_allocator().alloc(ZObjectSizeLimitSmall + 8);
    const uint64_t after = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
    const bool valid = result == 0 && after > before;
    std::fprintf(stderr, "MEDIUM_BLOCKING_TARGET result=%#zx occupied=%zu before=%llu after=%llu valid=%d\n",
                 result, occupiedBytes, (unsigned long long)before, (unsigned long long)after, valid);
    heap.RemoveExportObject(root);
    mutator->SetManagedContext(true);
    return reinterpret_cast<void*>(valid ? 0 : 1);
}
// ZGC zObjectAllocator.cpp:243-250: failed relocation allocation must not stall.
void* AllocateRelocationCapacity(void*)
{
    auto& heap = Heap::GetHeap();
    const size_t occupiedBytes = heap.GetMaxCapacity() - ZGranuleSize;
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(occupiedBytes - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    auto* occupied = MCC_NewObject(type, occupiedBytes);
    const U64 root = heap.RegisterExportRoot(occupied);
    Mutator* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    const uint64_t before = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
    const uintptr_t result = heap.object_allocator().alloc_for_relocation(heap.GetMaxCapacity(), PageAge::old);
    const uint64_t after = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
    const bool valid = result == 0 && after == before;
    std::fprintf(stderr, "RELOCATION_CAPACITY_TARGET result=%#zx before=%llu after=%llu valid=%d\n",
                 result, (unsigned long long)before, (unsigned long long)after, valid);
    heap.RemoveExportObject(root);
    mutator->SetManagedContext(true);
    return reinterpret_cast<void*>(valid ? 0 : 1);
}
void* AllocateNonBlockingCapacity(void*)
{
    auto& heap = Heap::GetHeap();
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZPage* occupied = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, true, PageAge::eden, flags);
    if (occupied == nullptr) { return reinterpret_cast<void*>(1); }
    Mutator* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    const uint64_t before = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
    // A valid page size that cannot fit while occupied consumes part of capacity.
    // The non-blocking allocation must return its failure without starting a GC.
    ZPage* result = Heap::alloc_page(heap.GetMaxCapacity(), ZPageType::large, false, true, PageAge::eden, flags);
    const uint64_t after = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
    const bool valid = result == nullptr && after == before;
    std::fprintf(stderr, "NONBLOCKING_CAPACITY_TARGET result=%p before=%llu after=%llu valid=%d\n",
                 result, (unsigned long long)before, (unsigned long long)after, valid);
    mutator->SetManagedContext(true);
    return reinterpret_cast<void*>(valid ? 0 : 2);
}
void* AllocateFastMedium(void*)
{
    auto& manager = Heap::GetHeap().page_allocator();
    if (!ZPageSizeMediumEnabled) { return reinterpret_cast<void*>(1); }
    const size_t size = ZPageSizeMediumMin;
    ZPage* cached = Heap::alloc_page(size, ZPageType::medium, false, false);
    if (cached == nullptr) { return reinterpret_cast<void*>(2); }
    Heap::free_page(cached);
    const size_t capacity = manager.GetCommittedBytes();
    const size_t before = manager.GetUsedRegionSize();
    ZAllocationFlags flags;
    flags.set_non_blocking();
    flags.set_fast_medium();
    ZPage* result = Heap::alloc_page(ZPageSizeMediumMax, ZPageType::medium, false, true, PageAge::eden, flags);
    const size_t actual = result == nullptr ? 0 : result->size();
    const bool valid = result != nullptr && actual == size && result->type() == ZPageType::medium &&
                       manager.GetCommittedBytes() == capacity && manager.GetUsedRegionSize() - before == actual;
    std::fprintf(stderr, "FAST_MEDIUM_TARGET min=%zu max=%zu actual=%zu capacity_before=%zu capacity_after=%zu valid=%d\n",
                 size, ZPageSizeMediumMax, actual, capacity, manager.GetCommittedBytes(), valid);
    return reinterpret_cast<void*>(valid ? 0 : 3);
}
}
GC_RUNTIME_OTHER_VM_TEST(ObjectAllocatorPaths, ManagedSizeRouting) { RunAllocatorCase(AllocateSizedObjects); }
GC_RUNTIME_OTHER_VM_TEST(ObjectAllocatorPaths, TLABsShareSmallPage) { RunAllocatorCase(AllocateTLABSlices); }
GC_RUNTIME_OTHER_VM_TEST(ObjectAllocatorPaths, FastMediumConsumesCachedActualSize) { RunAllocatorCase(AllocateFastMedium); }

GC_RUNTIME_OTHER_VM_TEST(ObjectAllocatorPaths, ManagedFastMediumConsumesCachedPage) { RunAllocatorCase(AllocateManagedFastMedium); }

GC_RUNTIME_OTHER_VM_TEST(ObjectAllocatorPaths, RelocationCapacityDoesNotStartCollection) { RunAllocatorCase(AllocateRelocationCapacity); }
GC_RUNTIME_OTHER_VM_TEST(ObjectAllocatorPaths, NonBlockingCapacityDoesNotStartCollection) { RunAllocatorCase(AllocateNonBlockingCapacity); }

GC_RUNTIME_OTHER_VM_TEST(ObjectAllocatorPaths, MediumNonBlockingAllocatesAfterCacheMiss) { RunAllocatorCase(AllocateMediumNonBlocking); }
GC_RUNTIME_OTHER_VM_TEST(ObjectAllocatorPaths, MediumBlockingFailureAttemptsCollection) { RunAllocatorCase(AllocateMediumBlockingFailure); }

#if defined(MRT_TESTABLE_INTERNALS)
GC_RUNTIME_OTHER_VM_TEST(ObjectAllocatorPaths, MediumBlockingFailureShutdownWithQueuedCollection)
{
    RunAllocatorCase(AllocateMediumBlockingFailure, true);
}

#endif

// #905: dirty backing is a fixture input, never an alternate implementation of
// allocation. MCC entries return the objects inspected below. Refilling must
// initialize the entire TLAB, while page/object allocation leaves its suffix alone.
namespace MapleRuntime {
extern "C" ArrayRef MCC_NewArray8(const TypeInfo*, MIndex);
extern "C" ObjRef MCC_NewFinalizer(const TypeInfo*, MSize);
}
namespace {
enum class ZeroCase { Page, TLAB, Medium, Large, Array, Finalizer, SegmentedArray };
bool BytesAre(uintptr_t begin, uintptr_t end, unsigned char value)
{
    for (; begin < end; ++begin) {
        if (*reinterpret_cast<const unsigned char*>(begin) != value) { return false; }
    }
    return true;
}

template<ZeroCase kind>
void* AllocateFromDirtyCache(void*)
{
    auto& heap = Heap::GetHeap();
    auto* buffer = AllocBuffer::GetAllocBuffer();
    buffer->RetireTLAB(false);
    heap.object_allocator().retire_pages(kPageAgeRangeEden);
    const bool small = kind == ZeroCase::TLAB;
    const bool medium = kind == ZeroCase::Medium || kind == ZeroCase::Array || kind == ZeroCase::Finalizer || kind == ZeroCase::SegmentedArray;
    const size_t bytes = small ? 256 : medium ? ZObjectSizeLimitSmall + 32 : ZObjectSizeLimitMedium + 32;
    const auto pageType = small ? ZPageType::small : medium ? ZPageType::medium : ZPageType::large;
    const size_t pageBytes = small ? ZPageSizeSmall : medium ? ZPageSizeMediumMin : AlignUp(bytes, ZGranuleSize);
    ZPage* seed = Heap::alloc_page(pageBytes, pageType, false, false);
    if (seed == nullptr) { return reinterpret_cast<void*>(1); }
    const uintptr_t base = seed->GetRegionStart();
    std::memset(reinterpret_cast<void*>(base), 0xa5, pageBytes);
    const bool seeded = BytesAre(base, base + pageBytes, 0xa5);
    Heap::free_page(seed);
    // Mapped-cache metadata is spread across the last granule. Select an
    // unchanged input span beyond the requested allocation, not that metadata.
    const size_t expectedInitialized = small ? buffer->ComputeTLABSize(bytes, heap.unsafe_max_tlab_alloc()) : bytes;
    size_t witness = expectedInitialized;
    while (witness + 64 <= pageBytes && !BytesAre(base + witness, base + witness + 64, 0xa5)) { witness += 64; }
    const bool dirtyWitness = witness + 64 <= pageBytes;

    alignas(TypeInfo) static unsigned char storage[3 * sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    auto* component = reinterpret_cast<TypeInfo*>(storage + sizeof(TypeInfo));
    auto* arrayType = reinterpret_cast<TypeInfo*>(storage + 2 * sizeof(TypeInfo));
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(bytes - TYPEINFO_PTR_SIZE);
    // A one-byte value struct uses ordinary array initialization; the primitive
    // case deliberately exercises the existing segmented initializer instead.
    component->SetType(kind == ZeroCase::Array ? TypeKind::TYPE_KIND_STRUCT : TypeKind::TYPE_KIND_UINT8);
    component->SetInstanceSize(1);
    arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    arrayType->SetComponentTypeInfo(component);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));

    uintptr_t object = 0;
    if (kind == ZeroCase::Page) {
        ZPage* reused = Heap::alloc_page(pageBytes, pageType, false, false);
        object = reused == nullptr ? 0 : reused->GetRegionStart();
        // Observe the dirty input span selected outside cache metadata.
        const bool preserved = object != 0 && dirtyWitness && BytesAre(object + witness, object + witness + 64, 0xa5);
        std::fprintf(stderr, "CACHE_ZERO_TARGET kind=page seeded=%d reused=%d preserved=%d\n",
                     seeded, object == base, preserved);
        const bool valid = seeded && object == base && preserved;
        if (reused != nullptr) { Heap::free_page(reused); }
        return reinterpret_cast<void*>(valid ? 0 : 2);
    }
    if (kind == ZeroCase::Array || kind == ZeroCase::SegmentedArray) {
        object = reinterpret_cast<uintptr_t>(MCC_NewArray8(arrayType, bytes - MArray::GetContentOffset()));
    } else if (kind == ZeroCase::Finalizer) {
        object = reinterpret_cast<uintptr_t>(MCC_NewFinalizer(type, bytes));
    } else {
        object = reinterpret_cast<uintptr_t>(MCC_NewObject(type, bytes));
    }
    const size_t initialized = small ? buffer->TLABSize() : bytes;
    const size_t header = (kind == ZeroCase::Array || kind == ZeroCase::SegmentedArray) ? MArray::GetContentOffset() : sizeof(BaseObject);
    const bool zero = object != 0 && BytesAre(object + header, object + initialized, 0);
    const bool suffix = object != 0 && dirtyWitness && initialized <= witness &&
                        BytesAre(object + witness, object + witness + 64, 0xa5);
    const bool reused = object == base;
    std::fprintf(stderr, "CACHE_ZERO_TARGET kind=%u seeded=%d reused=%d zero=%d suffix=%d initialized=%zu page=%zu\n",
                 unsigned(kind), seeded, reused, zero, suffix, initialized, pageBytes);
    return reinterpret_cast<void*>(seeded && reused && zero && suffix ? 0 : 3);
}
}
GC_RUNTIME_OTHER_VM_TEST(AllocationZeroing, CachedPagePreservesPayload)
{
    RunAllocatorCase(AllocateFromDirtyCache<ZeroCase::Page>);
}
GC_RUNTIME_OTHER_VM_TEST(AllocationZeroing, RefillClearsOnlyTLAB)
{
    RunAllocatorCase(AllocateFromDirtyCache<ZeroCase::TLAB>);
}
GC_RUNTIME_OTHER_VM_TEST(AllocationZeroing, MediumObjectClearsOnlyObject)
{
    RunAllocatorCase(AllocateFromDirtyCache<ZeroCase::Medium>);
}
GC_RUNTIME_OTHER_VM_TEST(AllocationZeroing, LargeObjectClearsOnlyObject)
{
    RunAllocatorCase(AllocateFromDirtyCache<ZeroCase::Large>);
}
GC_RUNTIME_OTHER_VM_TEST(AllocationZeroing, ArrayClearsOnlyObject)
{
    RunAllocatorCase(AllocateFromDirtyCache<ZeroCase::Array>);
}
GC_RUNTIME_OTHER_VM_TEST(AllocationZeroing, FinalizerClearsOnlyObject)
{
    RunAllocatorCase(AllocateFromDirtyCache<ZeroCase::Finalizer>);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationZeroing, SegmentedArrayKeepsItsOwnInitializer)
{
    RunAllocatorCase(AllocateFromDirtyCache<ZeroCase::SegmentedArray>);
}
namespace {
struct PageAllocationTiming {
    long long elapsedNs{0};
    size_t validPages{0};
};

void* AllocateWithoutPacing(void* argument)
{
    auto& timing = *static_cast<PageAllocationTiming*>(argument);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    const size_t size = ZObjectSizeLimitMedium + 8;
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(size - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    for (size_t i = 0; i < 3; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const auto object = reinterpret_cast<uintptr_t>(MCC_NewObject(type, size));
        timing.elapsedNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        const auto* page = object == 0 ? nullptr : Heap::page(object);
        timing.validPages += page != nullptr && page->IsLargeRegion() && page->size() >= size;
    }
    return nullptr;
}

void CheckNoPageAllocationPacing(bool slowConfiguration)
{
    // ZGC zPageAllocator.cpp:1401-1407 has no allocation-rate sleep before
    // allocating a page. Keep this workload far below capacity: it tests the
    // removed fixed pacing delay, not the legitimate allocation-stall path.
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    if (slowConfiguration) {
        // These legacy ABI fields are ignored by the product (cjcj#91).
        param.heapParam.allocationRate = 0.000001;
        param.heapParam.allocationWaitTime = 2000000000;
    }
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    PageAllocationTiming timing;
    CJThreadHandle handle = RunCJTask(AllocateWithoutPacing, &timing);
    GC_EXPECT_TRUE(handle != nullptr);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &result), E_OK);
    ReleaseHandle(handle);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
    // A restored pacing path sleeps for two seconds on each page request.
    // The one-second envelope distinguishes that deliberate delay; external
    // paired runs also compare the default/legacy configuration distributions.
    std::fprintf(stderr, "PAGE_PACING_TARGET slow=%d elapsed_ns=%lld pages=%zu samples=3\n",
                 slowConfiguration, timing.elapsedNs, timing.validPages);
    GC_EXPECT_TRUE(timing.elapsedNs < 1000000000LL);
    GC_EXPECT_EQ(timing.validPages, size_t{3});
}
}
GC_RUNTIME_OTHER_VM_TEST(PageAllocationPacing, DefaultConfiguration) { CheckNoPageAllocationPacing(false); }
GC_RUNTIME_OTHER_VM_TEST(PageAllocationPacing, LegacySlowConfiguration) { CheckNoPageAllocationPacing(true); }
