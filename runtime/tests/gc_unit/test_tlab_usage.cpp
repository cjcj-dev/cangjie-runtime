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
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "UnwindStack/StackFrameCursor.h"
#include "TypeInfoManager.h"
#include "gc_unittest.hpp"
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo* klass, MSize size);
}

GC_TEST(TLABUsage, BoundsAndDemand)
{
    AllocBuffer buffer;
    buffer.ClearRegion();
    std::fprintf(stderr, "TLAB_EMPTY_IDENTITY product=%p caller=%p\n",
                 static_cast<void*>(buffer.GetRegion()), static_cast<void*>(ZPage::NullRegion()));
    GC_EXPECT_TRUE(buffer.GetRegion() == nullptr);
    const size_t unit = 2 * 1024;
    const size_t maximum = ZObjectSizeLimitSmall;
    GC_EXPECT_EQ(buffer.ComputeTLABSize(0, maximum), unit);
    GC_EXPECT_EQ(buffer.ComputeTLABSize(unit, maximum), 2 * unit);
    GC_EXPECT_EQ(buffer.ComputeTLABSize(maximum, maximum), maximum);
    GC_EXPECT_EQ(buffer.ComputeTLABSize(maximum + 1, maximum), size_t{0});
    GC_EXPECT_EQ(buffer.ComputeTLABSize(std::numeric_limits<size_t>::max(), maximum), size_t{0});
    GC_EXPECT_EQ(buffer.ComputeTLABSize(1, 0), size_t{0});
}

#if defined(__linux__)
namespace {
void NativeFrameProbe() {}

void CheckNativeFrameScan(bool derived)
{
    const auto* pc = reinterpret_cast<const uint32_t*>(&NativeFrameProbe);
    GC_EXPECT_TRUE(MFuncDesc::GetFuncDesc(reinterpret_cast<Uptr>(pc)) == nullptr);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        FrameInfo frame(pc);
        frame.mFrame.SetIP(pc);
        Mutator mutator;
        RegSlotsMap registers;
        size_t visits = 0;
        const RootVisitor roots = [&](RootSlot&) { ++visits; };
        const DerivedPtrVisitor derivedRoots = [&](BasePtrType, DerivedSlot&) { ++visits; };
        StackFrameCursor::ProcessManagedFrame(roots, derived ? &derivedRoots : nullptr, registers, frame, mutator);
        _exit(visits == 0 ? 0 : 1);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const bool completed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::fprintf(stderr, "NATIVE_FRAME_TARGET executed=1 derived=%d status=%d completed=%d\n",
                 derived, status, completed);
    GC_EXPECT_TRUE(completed);
}
}

GC_TEST(TLABUsage, NativeFrameRootScan)
{
    CheckNativeFrameScan(false);
}

GC_TEST(TLABUsage, NativeFrameDerivedScan)
{
    CheckNativeFrameScan(true);
}
#endif

// zPageAllocator.cpp:1375-1393: occupancy is increase/decrease_used_generation
// and promote_used, not ZPage::reset()'s youngRegionBytes side counter.
GC_OTHER_VM_TEST(TLABUsage, YoungOccupancyUsesActualExtent)
{
    auto& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    const size_t beforeYoung = manager.used_generation(ZGenerationId::young);
    const size_t beforeOld = manager.used_generation(ZGenerationId::old);
    const size_t small = ZGranuleSize;
    const size_t two = 2 * ZGranuleSize;
    manager.increase_used_generation(ZGenerationId::young, small + two);
    GC_EXPECT_EQ(manager.GetYoungAllocatedSize() - beforeYoung, small + two);
    manager.decrease_used_generation(ZGenerationId::young, two);
    manager.increase_used_generation(ZGenerationId::old, two);
    GC_EXPECT_EQ(manager.GetYoungAllocatedSize() - beforeYoung, small);
    GC_EXPECT_EQ(manager.used_generation(ZGenerationId::old) - beforeOld, two);
    manager.decrease_used_generation(ZGenerationId::young, small);
    manager.decrease_used_generation(ZGenerationId::old, two);
    GC_EXPECT_EQ(manager.GetYoungAllocatedSize(), beforeYoung);
    GC_EXPECT_EQ(manager.used_generation(ZGenerationId::old), beforeOld);
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
    const size_t maximum = ZObjectSizeLimitSmall;
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
    buffer = AllocBuffer::GetOrCreateAllocBuffer();
    const size_t initial = buffer->ComputeTLABSize(objectSize, maximum);
    size_t backingBytes = 0;
    size_t requestedBytes = 0;
    ZPage* previous = nullptr;
    for (size_t bytes = 0; bytes < 2 * MB; bytes += objectSize) {
        if (MCC_NewObject(type, objectSize) == nullptr) {
            return reinterpret_cast<void*>(1);
        }
        ZPage* current = buffer->GetRegion();
        if (current != previous) {
            backingBytes += current->GetRegionSize();
            previous = current;
        }
        requestedBytes += objectSize;
    }
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
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
    const size_t actual = buffer->TLABSize();
    // ZHeap::alloc_tlab reserves a slice from a shared small page.
    const bool valid = initial >= objectSize && computed >= objectSize &&
                       actual == computed && computed <= maximum &&
                       buffer->GetRegion()->GetRegionSize() == ZPageSizeSmall;
    std::fprintf(stderr, "TLAB_CYCLE initial=%zu computed=%zu refill=%zu maximum=%zu valid=%u\n",
                 initial, computed, actual, maximum, static_cast<unsigned>(valid));
    return reinterpret_cast<void*>(valid ? 0 : 3);
}
}

GC_OTHER_VM_TEST(TLABUsage, AllocationCycleKeepsGranuleBacking)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
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
