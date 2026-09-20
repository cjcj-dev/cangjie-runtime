// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include <cstring>
#include "Cangjie.h"
#include "gc_unittest.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zGlobals.hpp"
#include "TypeInfoManager.h"
#include "Mutator/Mutator.h"

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
static void RunAllocatorCase(CJTaskFunc task)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    CJThreadHandle handle = RunCJTask(task, nullptr);
    GC_EXPECT_TRUE(handle != nullptr);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &result), E_OK);
    ReleaseHandle(handle);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(result), uintptr_t{0});
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
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
    auto* buffer = AllocBuffer::GetOrCreateAllocBuffer();
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
void* AllocateNonBlockingCapacity(void*)
{
    auto& heap = Heap::GetHeap();
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZPage* occupied = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, true, false, PageAge::eden, flags);
    if (occupied == nullptr) { return reinterpret_cast<void*>(1); }
    Mutator* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    const uint64_t before = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
    // A valid page size that cannot fit while occupied consumes part of capacity.
    // The non-blocking allocation must return its failure without starting a GC.
    ZPage* result = Heap::alloc_page(heap.GetMaxCapacity(), ZPageType::large, false, true, false, PageAge::eden, flags);
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
    ZPage* result = Heap::alloc_page(ZPageSizeMediumMax, ZPageType::medium, false, true, true, PageAge::eden, flags);
    const size_t actual = result == nullptr ? 0 : result->size();
    const bool valid = result != nullptr && actual == size && result->type() == ZPageType::medium &&
                       manager.GetCommittedBytes() == capacity && manager.GetUsedRegionSize() - before == actual;
    std::fprintf(stderr, "FAST_MEDIUM_TARGET min=%zu max=%zu actual=%zu capacity_before=%zu capacity_after=%zu valid=%d\n",
                 size, ZPageSizeMediumMax, actual, capacity, manager.GetCommittedBytes(), valid);
    return reinterpret_cast<void*>(valid ? 0 : 3);
}
}
GC_OTHER_VM_TEST(ObjectAllocatorPaths, ManagedSizeRouting) { RunAllocatorCase(AllocateSizedObjects); }
GC_OTHER_VM_TEST(ObjectAllocatorPaths, TLABsShareSmallPage) { RunAllocatorCase(AllocateTLABSlices); }
GC_OTHER_VM_TEST(ObjectAllocatorPaths, FastMediumConsumesCachedActualSize) { RunAllocatorCase(AllocateFastMedium); }

GC_OTHER_VM_TEST(ObjectAllocatorPaths, ManagedFastMediumConsumesCachedPage) { RunAllocatorCase(AllocateManagedFastMedium); }

GC_OTHER_VM_TEST(ObjectAllocatorPaths, NonBlockingCapacityDoesNotStartCollection) { RunAllocatorCase(AllocateNonBlockingCapacity); }
