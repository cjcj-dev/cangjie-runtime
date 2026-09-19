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
        const auto* buffer = AllocBuffer::GetAllocBuffer();
        const bool tlab = i != 0 || (buffer != nullptr && buffer->TLABSize() >= size && buffer->GetRegion() == page);
        std::fprintf(stderr, "OBJECT_ALLOCATOR_TARGET branch=%zu size=%zu page_size=%zu type=%d sharing=%d distinct=%d tlab=%d\n",
                     i, size, page == nullptr ? 0 : page->GetRegionSize(), correctType, shared, distinctObjects, tlab);
        if (!correctType || !shared || !distinctObjects || !tlab) { return reinterpret_cast<void*>(2 + i); }
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
    constexpr size_t bytes = 2048;
    const uintptr_t first = Heap::GetHeap().alloc_tlab(bytes);
    const uintptr_t second = Heap::GetHeap().alloc_tlab(bytes);
    const bool shared = first != 0 && second == first + bytes && Heap::page(first) == Heap::page(second);
    std::fprintf(stderr, "TLAB_SHARED_TARGET first=%#zx second=%#zx shared=%d bytes=%zu\n", first, second, shared, bytes);
    return reinterpret_cast<void*>(shared ? 0 : 1);
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
