// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// HotSpot ThreadLocalAllocBuffer::compute_size/resize, and allocation-cycle
// coverage corresponding to gc/shenandoah/TestResizeTLAB.java (shared TLAB
// implementation). The ZGC test directory has no dedicated TLAB sizing test.
// History is produced only by MCC_NewObject and a real young collection.
#include <cstring>
#include <limits>
#include "Cangjie.h"
#include "gc_heap_fixture.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/Collector.h"
#include "TypeInfoManager.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo* klass, MSize size);
}

GC_TEST(TLABUsage, BoundsAndDemand)
{
    AllocBuffer buffer;
    const size_t unit = RegionInfo::UNIT_SIZE;
    const size_t maximum = 32 * unit;
    GC_EXPECT_EQ(buffer.ComputeTLABSize(0, maximum), unit);
    GC_EXPECT_EQ(buffer.ComputeTLABSize(unit, maximum), 2 * unit);
    GC_EXPECT_EQ(buffer.ComputeTLABSize(maximum, maximum), maximum);
    GC_EXPECT_EQ(buffer.ComputeTLABSize(maximum + 1, maximum), size_t{0});
    GC_EXPECT_EQ(buffer.ComputeTLABSize(std::numeric_limits<size_t>::max(), maximum), size_t{0});
    GC_EXPECT_EQ(buffer.ComputeTLABSize(1, 0), size_t{0});
}

// ZPageAllocator::increase_used_generation/decrease_used_generation:
// occupancy changes by the page extent, independently of the TLAB maximum.
GC_OTHER_VM_TEST(TLABUsage, YoungOccupancyUsesActualExtent)
{
    GcHeapFixture fixture;
    auto& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    RegionInfo* twoUnits = RegionInfo::InitRegion(2, 2, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    const size_t before = manager.GetYoungAllocatedSize();
    fixture.region0->SetYoungRegionFlag(1);
    twoUnits->SetYoungRegionFlag(1);
    GC_EXPECT_EQ(manager.GetYoungAllocatedSize() - before, 3 * RegionInfo::UNIT_SIZE);
    twoUnits->SetYoungRegionFlag(0);
    GC_EXPECT_EQ(manager.GetYoungAllocatedSize() - before, RegionInfo::UNIT_SIZE);
    fixture.region0->SetYoungRegionFlag(0);
    GC_EXPECT_EQ(manager.GetYoungAllocatedSize(), before);
}

#if defined(__linux__)
namespace {
void* AllocateThroughCycle(void*)
{
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)];
    std::memset(storage, 0, sizeof(storage));
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(256);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    const MSize objectSize = 256 + TYPEINFO_PTR_SIZE;
    auto& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    AllocBuffer* buffer = AllocBuffer::GetOrCreateAllocBuffer();
    const size_t maximum = manager.GetThreadLocalRegionSize();
    Heap::GetHeap().GetCollector().RequestGC(GC_REASON_YOUNG, false);
    buffer = AllocBuffer::GetOrCreateAllocBuffer();
    const size_t initial = buffer->ComputeTLABSize(objectSize, maximum);
    size_t backingBytes = 0;
    size_t requestedBytes = 0;
    RegionInfo* previous = nullptr;
    for (size_t bytes = 0; bytes < 2 * MB; bytes += objectSize) {
        if (MCC_NewObject(type, objectSize) == nullptr) {
            return reinterpret_cast<void*>(1);
        }
        RegionInfo* current = buffer->GetRegion();
        if (current != previous) {
            backingBytes += current->GetRegionSize();
            previous = current;
        }
        requestedBytes += objectSize;
    }
    Heap::GetHeap().GetCollector().RequestGC(GC_REASON_YOUNG, false);
    buffer = AllocBuffer::GetOrCreateAllocBuffer();
    // ZHeap::account_alloc_page: the cycle denominator retains backing
    // capacity, including unused TLAB tails. These values come from actual
    // MCC_NewObject refills, never a test history setter.
    const size_t cycleBacking = manager.GetTLABUsed();
    if (backingBytes <= requestedBytes || cycleBacking < backingBytes) {
        std::fprintf(stderr, "TLAB_BACKING requested=%zu observed=%zu cycle=%zu\n",
                     requestedBytes, backingBytes, cycleBacking);
        return reinterpret_cast<void*>(4);
    }
    const size_t computed = buffer->ComputeTLABSize(objectSize, maximum);
    if (MCC_NewObject(type, objectSize) == nullptr) {
        return reinterpret_cast<void*>(2);
    }
    const size_t actual = buffer->GetRegion()->GetRegionSize();
    const bool valid = computed > initial && computed <= maximum && actual == computed;
    std::fprintf(stderr, "TLAB_CYCLE initial=%zu computed=%zu refill=%zu maximum=%zu valid=%u\n",
                 initial, computed, actual, maximum, static_cast<unsigned>(valid));
    return reinterpret_cast<void*>(valid ? 0 : 3);
}
}

GC_OTHER_VM_TEST(TLABUsage, AllocationCycleResizesNextRefill)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 32 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    CJThreadHandle handle = RunCJTask(AllocateThroughCycle, nullptr);
    GC_EXPECT_TRUE(handle != nullptr);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &result), E_OK);
    ReleaseHandle(handle);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(result), uintptr_t{0});
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
#endif
