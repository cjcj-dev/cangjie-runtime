// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// HotSpot ThreadLocalAllocBuffer::compute_size/resize, and allocation-cycle
// coverage corresponding to gc/shenandoah/TestResizeTLAB.java (shared TLAB
// implementation). The ZGC test directory has no dedicated TLAB sizing test.
// History is produced only by MCC_NewObject and a real young collection.
#define MRT_USE_CJTHREAD_RENAME 1
#include <cstring>
#include <limits>
#include <fstream>
#include "Mutator/ThreadSMR.h"
#include "Cangjie.h"
#include "gc_heap_fixture.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/shared/collectedHeap.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "UnwindStack/StackFrameCursor.h"
#include "TypeInfoManager.h"
#include "gc_unittest.hpp"
#if defined(__linux__)
#include <sys/syscall.h>
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
// ZGC zStackWatermark.cpp:187-192. A real phase flip publishes the new
// masks; the root task installs them on each logical owner.
GC_RUNTIME_OTHER_VM_TEST(ThreadStoreMask, YoungPhasePublishesToOwners)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    std::atomic<Mutator*> owner{nullptr};
    ThreadLocalData* carrier = nullptr;
    std::atomic<bool> finish{false};
    std::thread thread([&] {
        auto& manager = MutatorManager::Instance();
        Mutator* current = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        carrier = ThreadLocal::GetThreadLocalData();
        owner.store(current, std::memory_order_release);
        while (!finish.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    while (owner.load(std::memory_order_acquire) == nullptr) { std::this_thread::yield(); }
    Mutator* current = owner.load();
    const uintptr_t before = current->GetGCData().storeBadMask;
    auto& young = Heap::GetHeap().young();
    young.Workers()->set_active_workers(1);
    young.Begin(1);
    young.pause_mark_start();
    const uintptr_t published = ZPointerStoreBadMask;
    ZMark::VisitMinorRoots([](BaseObject*) {}, [](BaseObject*) {});
    const uintptr_t after = current->GetGCData().storeBadMask;
    // Consume the carrier slot and field through the fixed target ABI, as the
    // paired compiler lowering does. Do not derive either offset from offsetof.
    const auto* byCarrier = *reinterpret_cast<ThreadGCData* const*>(
        reinterpret_cast<const unsigned char*>(carrier) + ThreadGCDataABI::GCDataPointer);
    const bool sameOwner = byCarrier == &current->GetGCData();
    const uintptr_t byOffset = *reinterpret_cast<const uintptr_t*>(
        reinterpret_cast<const unsigned char*>(byCarrier) + ThreadGCDataABI::StoreBadMask);
    finish.store(true, std::memory_order_release);
    thread.join();
    std::fprintf(stderr, "THREAD_STORE_MASK_TARGET executed=1 before=%zx published=%zx after=%zx offset_value=%zx same_owner=%d\n",
                 before, published, after, byOffset, sameOwner);
    GC_EXPECT_TRUE(before != published && after == published && byOffset == published && sameOwner);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
void NativeFrameProbe() {}

void CheckNativeFrameScan(bool derived)
{
    const auto* pc = reinterpret_cast<const uint32_t*>(&NativeFrameProbe);
    GC_EXPECT_TRUE(MFuncDesc::GetFuncDesc(reinterpret_cast<Uptr>(pc)) == nullptr);
    FrameInfo frame(pc);
    frame.mFrame.SetIP(pc);
    Mutator mutator;
    RegSlotsMap registers;
    size_t visits = 0;
    const RootVisitor roots = [&](RootSlot&) { ++visits; };
    const DerivedPtrVisitor derivedRoots = [&](BasePtrType, DerivedSlot&) { ++visits; };
    StackFrameCursor::ProcessManagedFrame(roots, derived ? &derivedRoots : nullptr, registers, frame, mutator);
    std::fprintf(stderr, "NATIVE_FRAME_TARGET executed=1 derived=%d visits=%zu\n", derived, visits);
    GC_EXPECT_EQ(visits, size_t{0});
}
}

GC_OTHER_VM_TEST(TLABUsage, NativeFrameRootScan)
{
    CheckNativeFrameScan(false);
}

GC_OTHER_VM_TEST(TLABUsage, NativeFrameDerivedScan)
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
    AllocBuffer* buffer = AllocBuffer::GetAllocBuffer();
    const size_t maximum = ZObjectSizeLimitSmall;
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
    buffer = AllocBuffer::GetAllocBuffer();
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
    buffer = AllocBuffer::GetAllocBuffer();
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

GC_RUNTIME_OTHER_VM_TEST(TLABUsage, AllocationCycleKeepsGranuleBacking)
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

#if defined(__linux__)
namespace {
void* AllocateInlineBounds(void*)
{
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)];
    std::memset(storage, 0, sizeof(storage));
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    constexpr size_t bytes = 256;
    type->SetInstanceSize(bytes - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    AllocBuffer* buffer = AllocBuffer::GetAllocBuffer();
    const size_t requested = buffer->ComputeTLABSize(bytes, Heap::GetHeap().unsafe_max_tlab_alloc());
    size_t fits = 0;
    size_t refills = 0;
    // Read the actual ABI words consumed by generated code. No layout copy is
    // populated by this test: all state comes from MCC_NewObject/FillTLAB.
    for (size_t i = 0; i < requested / bytes + 2; ++i) {
        uintptr_t before[3];
        std::memcpy(before, buffer, sizeof(before));
        const uintptr_t object = reinterpret_cast<uintptr_t>(MCC_NewObject(type, bytes));
        uintptr_t after[3];
        std::memcpy(after, buffer, sizeof(after));
        const bool fitsBefore = before[0] != 0 && before[0] <= before[1] && bytes <= before[1] - before[0];
        const bool bounds = object != 0 && after[2] <= object && object <= after[1] &&
                            bytes <= after[1] - object && after[0] == object + bytes &&
                            after[1] - after[2] == requested;
        const bool advance = !fitsBefore || (object == before[0] && after[1] == before[1]);
        std::fprintf(stderr, "TLAB_INLINE_BOUNDS_TARGET iteration=%zu object=%#zx top=%#zx end=%#zx start=%#zx fits=%d bounds=%d advance=%d\n",
                     i, object, after[0], after[1], after[2], fitsBefore, bounds, advance);
        if (!bounds || !advance) { return reinterpret_cast<void*>(1); }
        fits += fitsBefore;
        refills += !fitsBefore;
    }
    std::fprintf(stderr, "TLAB_INLINE_BRANCHES fits=%zu refills=%zu\n", fits, refills);
    return reinterpret_cast<void*>(fits > 0 && refills > 0 ? 0 : 2);
}
}

GC_OTHER_VM_TEST(TLABUsage, InlineBoundsThroughAllocationEntry)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    CJThreadHandle handle = RunCJTask(AllocateInlineBounds, nullptr);
    GC_EXPECT_TRUE(handle != nullptr);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &result), E_OK);
    ReleaseHandle(handle);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(result), uintptr_t{0});
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
#endif

#if defined(__linux__)
#include <atomic>
#include <chrono>
#include <thread>
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "Heap/z/zWorkers.hpp"
#include "schedule.h"
extern "C" int CJ_CJThreadResched();

namespace {
TypeInfo* TLABTestType()
{
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(4096 - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    return type;
}

struct TLABOwnerCase {
    TypeInfo* type;
    std::atomic<unsigned> turn{0};
    AllocBuffer* first = nullptr;
    AllocBuffer* second = nullptr;
    bool stable = false;
    bool distinct = false;
    bool firstBinding = false;
    bool secondBinding = false;
};
void* FirstTLABOwner(void* argument)
{
    auto& state = *static_cast<TLABOwnerCase*>(argument);
    (void)MCC_NewObject(state.type, 4096);
    state.first = AllocBuffer::GetAllocBuffer();
    state.firstBinding = state.first == ThreadLocal::GetThreadLocalData()->buffer;
    state.turn.store(1, std::memory_order_release);
    while (state.turn.load(std::memory_order_acquire) != 2) { CJ_CJThreadResched(); }
    state.stable = AllocBuffer::GetAllocBuffer() == state.first;
    state.distinct = state.first != state.second;
    (void)MCC_NewObject(state.type, 4096);
    state.turn.store(3, std::memory_order_release);
    return nullptr;
}
void* SecondTLABOwner(void* argument)
{
    auto& state = *static_cast<TLABOwnerCase*>(argument);
    while (state.turn.load(std::memory_order_acquire) != 1) { CJ_CJThreadResched(); }
    (void)MCC_NewObject(state.type, 4096);
    state.second = AllocBuffer::GetAllocBuffer();
    state.secondBinding = state.second == ThreadLocal::GetThreadLocalData()->buffer;
    state.turn.store(2, std::memory_order_release);
    while (state.turn.load(std::memory_order_acquire) != 3) { CJ_CJThreadResched(); }
    return nullptr;
}
}

// HotSpot Thread::_tlab (thread.hpp:258,406) belongs to a logical thread.
// Two real tasks alternate on the same worker through the product scheduler.
GC_RUNTIME_OTHER_VM_TEST(TLABOwnership, ParkResumeKeepsExclusiveBuffer)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    TLABOwnerCase state{TLABTestType()};
    auto first = RunCJTask(FirstTLABOwner, &state);
    auto second = RunCJTask(SecondTLABOwner, &state);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(first, &result), E_OK);
    GC_EXPECT_EQ(GetTaskRet(second, &result), E_OK);
    ReleaseHandle(first);
    ReleaseHandle(second);
    std::fprintf(stderr, "TLAB_OWNER_TARGET executed=1 first=%p second=%p stable=%d distinct=%d bindings=%d,%d\n",
                 state.first, state.second, state.stable, state.distinct, state.firstBinding, state.secondBinding);
    GC_EXPECT_TRUE(state.stable && state.distinct && state.firstBinding && state.secondBinding);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
struct TLABSnapshotCase {
    TypeInfo* type;
    std::atomic<Mutator*> owner[2]{};
    std::atomic<int> refillOwner{-1};
    std::atomic<uint32_t> epoch{0};
    std::atomic<bool> refilled{false};
    std::atomic<bool> published{false};
    TLABStatistics pending;
};
void SnapshotOwner(TLABSnapshotCase& state, unsigned index)
{
    auto& manager = MutatorManager::Instance();
    Mutator* owner = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    owner->DoLeaveSaferegion();
    for (unsigned i = 0; i < 128; ++i) { (void)MCC_NewObject(state.type, 4096); }
    owner->DoEnterSaferegion();
    state.owner[index].store(owner, std::memory_order_release);
    while (state.epoch.load(std::memory_order_acquire) == 0) { std::this_thread::yield(); }
    if (state.refillOwner.load(std::memory_order_acquire) == static_cast<int>(index)) {
        while (!owner->GetStackWatermark().IsDone(state.epoch.load())) { std::this_thread::yield(); }
        owner->DoLeaveSaferegion();
        for (unsigned i = 0; i < 32; ++i) { (void)MCC_NewObject(state.type, 4096); }
        owner->DoEnterSaferegion();
        state.refilled.store(true, std::memory_order_release);
    }
    while (!state.published.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    if (state.refillOwner.load() == static_cast<int>(index)) {
        // Read the owner's remaining product statistics only after publication.
        // They are assertion output, never injected into a downstream phase.
        owner->tlab()->AccumulateTLABStatistics(state.pending,
            Heap::GetHeap().page_allocator().GetTLABUsed(), Heap::GetHeap().page_allocator().GetTLABCapacity());
    }
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
}
}

// ZGC zStackWatermark.cpp:197 and zMark.cpp:703: a retired snapshot is
// consumed after finish_processing, while that owner may already allocate again.
static void CheckRootPublicationPreservesLaterRefills(unsigned workers)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    TLABSnapshotCase state{TLABTestType()};
    std::thread a([&] { SnapshotOwner(state, 0); });
    std::thread b([&] { SnapshotOwner(state, 1); });
    while (state.owner[0].load(std::memory_order_acquire) == nullptr ||
           state.owner[1].load(std::memory_order_acquire) == nullptr) { std::this_thread::yield(); }
    auto& heap = Heap::GetHeap();
    heap.young().Workers()->set_active_workers(workers);
    heap.young().Begin(1);
    heap.young().pause_mark_start();
    const auto initialTLABSize = [] {
        size_t size = 0;
        std::thread probe([&] {
            auto* owner = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
            size = owner->tlab()->ComputeTLABSize(0, ZObjectSizeLimitSmall);
            MutatorManager::Instance().DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        });
        probe.join();
        return size;
    };
    // Keep the capacity history identical on both sides: mark-start already
    // reset it, and only root-worker publication may change this next sample.
    const size_t beforePublication = initialTLABSize();
    // Choose using the same product inventory order as JavaThreadsIterator.
    int first = -1;
    MutatorManager::Instance().VisitAllMutators([&](Mutator& owner) {
        for (int i = 0; i < 2; ++i) {
            if (&owner == state.owner[i].load() && first == -1) { first = i; }
        }
    });
    Mutator* last = state.owner[1 - first].load();
    last->MutatorLock();
    state.refillOwner.store(first, std::memory_order_release);
    state.epoch.store(StackWatermark::epoch_id(), std::memory_order_release);
    std::thread roots([&] { ZMark::VisitMinorRoots([](BaseObject*) {}, [](BaseObject*) {}); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!state.refilled.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool overlapped = state.refilled.load(std::memory_order_acquire);
    last->MutatorUnlock();
    roots.join();
    TLABStatistics retired;
    for (auto& owner : state.owner) { retired.Update(owner.load()->GetStackWatermark().stats()); }
    // A fresh thread's initial size consumes the published worker totals.
    // Both allocating owners have real allocation history; no history setter.
    const size_t initial = initialTLABSize();
    state.published.store(true, std::memory_order_release);
    a.join();
    b.join();
    std::fprintf(stderr, "TLAB_SNAPSHOT_TARGET executed=1 overlap=%d retired=%zu threads=%zu pending=%zu initial=%zu before_publication=%zu\n",
                 overlapped, retired.allocatedSize, retired.allocatingThreads, state.pending.allocatedSize, initial, beforePublication);
    GC_EXPECT_TRUE(overlapped);
    GC_EXPECT_TRUE(retired.allocatedSize > 0 && retired.allocatingThreads == 2);
    GC_EXPECT_TRUE(state.pending.allocatedSize > 0);
    // threadLocalAllocBuffer.cpp:324 seeds the averages at startup. Test
    // the published history's observable effect, not a reconstructed average.
    GC_EXPECT_TRUE(initial > beforePublication);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(TLABSnapshot, RootPublicationPreservesLaterRefills)
{
    CheckRootPublicationPreservesLaterRefills(1);
}

GC_RUNTIME_OTHER_VM_TEST(TLABSnapshot, ParallelRootPublicationPreservesLaterRefills)
{
    CheckRootPublicationPreservesLaterRefills(2);
}

#endif

#if defined(__linux__)
// Exercise the scheduler's actual bind/resume entry on two OS workers. The
// first worker stays alive until the second has resumed the parked owner.
GC_RUNTIME_OTHER_VM_TEST(TLABOwnership, ResumeOnAnotherWorkerKeepsBuffer)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    TypeInfo* type = TLABTestType();
    std::atomic<Mutator*> parked{nullptr};
    std::atomic<bool> complete{false};
    AllocBuffer* before = nullptr;
    AllocBuffer* after = nullptr;
    uintptr_t allocated = 0;
    std::thread source([&] {
        auto& manager = MutatorManager::Instance();
        Mutator* owner = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        owner->DoLeaveSaferegion();
        (void)MCC_NewObject(type, 4096);
        before = owner->tlab();
        owner->PreparedToPark(nullptr, nullptr);
        manager.UnbindMutator(*owner);
        parked.store(owner, std::memory_order_release);
        while (!complete.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        manager.UnregisterMarkFlushThread(ThreadLocal::GetThreadLocalData());
    });
    std::thread destination([&] {
        while (parked.load(std::memory_order_acquire) == nullptr) { std::this_thread::yield(); }
        auto* owner = parked.load();
        auto& manager = MutatorManager::Instance();
        manager.BindMutator(*owner);
        owner->PreparedToRun(ThreadLocal::GetThreadLocalData());
        after = AllocBuffer::GetAllocBuffer();
        allocated = reinterpret_cast<uintptr_t>(MCC_NewObject(type, 4096));
        owner->DoEnterSaferegion();
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        complete.store(true, std::memory_order_release);
    });
    source.join();
    destination.join();
    std::fprintf(stderr, "TLAB_MIGRATION_TARGET executed=1 before=%p after=%p allocated=%#zx\n", before, after, allocated);
    GC_EXPECT_TRUE(before == after && allocated != 0);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
#endif

#if defined(__linux__)
#include <climits>
#include "waitqueue.h"
namespace {
struct ParkedTLABCase {
    TypeInfo* type;
    Waitqueue release{};
    std::atomic<bool> parked{false}, ready{false}, flipped{false}, refilled{false}, rootsDone{false}, finish{false};
    size_t before = 0;
    size_t after = 0;
    Mutator* parkedOwner = nullptr;
    size_t parkedBefore = 0;
};
bool ParkedTLABReleased(void* value) { return static_cast<ParkedTLABCase*>(value)->finish.load(); }
void* ParkOldTLABOwner(void* value)
{
    auto& state = *static_cast<ParkedTLABCase*>(value);
    (void)MCC_NewObject(state.type, 4096);
    state.parkedOwner = Mutator::GetMutator();
    state.parkedBefore = state.parkedOwner->tlab()->TLABSize();
    state.parked.store(true, std::memory_order_release);
    WaitqueuePark(&state.release, LLONG_MAX, ParkedTLABReleased, &state, false);
    return nullptr;
}
void* RunNewTLABOwner(void* value)
{
    auto& state = *static_cast<ParkedTLABCase*>(value);
    while (!state.parked.load(std::memory_order_acquire)) { CJ_CJThreadResched(); }
    auto* owner = Mutator::GetMutator();
    (void)MCC_NewObject(state.type, 4096);
    owner->DoEnterSaferegion();
    state.ready.store(true, std::memory_order_release);
    while (!state.flipped.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    owner->DoLeaveSaferegion(); // Process this owner's new epoch before refill.
    (void)MCC_NewObject(state.type, 4096);
    state.before = AllocBuffer::GetAllocBuffer()->TLABSize();
    owner->DoEnterSaferegion();
    state.refilled.store(true, std::memory_order_release);
    while (!state.rootsDone.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    state.after = AllocBuffer::GetAllocBuffer()->TLABSize();
    owner->DoLeaveSaferegion();
    state.finish.store(true, std::memory_order_release);
    WaitqueueWakeAll(&state.release, nullptr, nullptr);
    return nullptr;
}
}
GC_RUNTIME_OTHER_VM_TEST(TLABOwnership, ParkedRootDoesNotRetireRunningOwner)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ParkedTLABCase state{TLABTestType()};
    GC_EXPECT_EQ(WaitqueueNew(&state.release), 0);
    auto first = RunCJTask(ParkOldTLABOwner, &state);
    auto second = RunCJTask(RunNewTLABOwner, &state);
    while (!state.ready.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    auto& young = Heap::GetHeap().young();
    young.Begin(1);
    young.pause_mark_start();
    state.flipped.store(true, std::memory_order_release);
    while (!state.refilled.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    ZMark::VisitMinorRoots([](BaseObject*) {}, [](BaseObject*) {});
    const size_t parkedAfter = state.parkedOwner->tlab()->TLABSize();
    state.rootsDone.store(true, std::memory_order_release);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(first, &result), E_OK);
    GC_EXPECT_EQ(GetTaskRet(second, &result), E_OK);
    ReleaseHandle(first);
    ReleaseHandle(second);
    WaitqueueDelete(&state.release);
    std::fprintf(stderr, "TLAB_PARKED_OWNER_TARGET executed=1 before=%zu after=%zu parked_before=%zu parked_after=%zu\n",
                 state.before, state.after, state.parkedBefore, parkedAfter);
    GC_EXPECT_TRUE(state.before > 0 && state.after == state.before && state.parkedBefore > 0 && parkedAfter == 0);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
#endif

#if defined(__linux__)
namespace {
bool WaitForSMR(const std::function<bool()>& done)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < end) { std::this_thread::yield(); }
    return done();
}
struct ThreadSnapshotCase {
    TypeInfo* type;
    std::atomic<Mutator*> owner[2]{};
    std::atomic<BaseObject*> marker{nullptr};
    std::atomic<bool> exitTarget{false};
    std::atomic<bool> exited{false};
    std::atomic<bool> finish{false};
    std::atomic<pid_t> targetTid{0};
};
void SnapshotExitOwner(ThreadSnapshotCase& state, unsigned index)
{
    auto& manager = MutatorManager::Instance();
    auto* owner = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    owner->DoLeaveSaferegion();
    BaseObject* object = nullptr;
    for (unsigned i = 0; i < 128; ++i) { object = reinterpret_cast<BaseObject*>(MCC_NewObject(state.type, 4096)); }
    const size_t mark = owner->NativeFrameRootCount();
    owner->AddNativeFrameRoot(object);
    owner->DoEnterSaferegion();
    if (index == 0) { state.marker.store(object, std::memory_order_release); }
    else { state.targetTid.store(static_cast<pid_t>(syscall(SYS_gettid))); }
    state.owner[index].store(owner, std::memory_order_release);
    while (!(index == 1 ? state.exitTarget.load() : state.finish.load())) { std::this_thread::yield(); }
    owner->PopNativeFrameRootsTo(mark);
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    if (index == 1) { state.exited.store(true, std::memory_order_release); }
}

void CheckConcurrentRootSnapshot(bool exitDuringRoots, bool nested = false)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ThreadSnapshotCase state{TLABTestType()};
    std::thread first([&] { SnapshotExitOwner(state, 0); });
    GC_EXPECT_TRUE(WaitForSMR([&] { return state.owner[0].load() != nullptr; }));
    std::thread target([&] { SnapshotExitOwner(state, 1); });
    GC_EXPECT_TRUE(WaitForSMR([&] { return state.owner[1].load() != nullptr; }));
    Mutator* identity = state.owner[1].load();
    auto& young = Heap::GetHeap().young();
    young.Workers()->set_active_workers(1);
    young.Begin(1);
    young.pause_mark_start();
    const uint32_t epoch = StackWatermark::epoch_id();
    std::atomic<bool> rootObserved{false};
    std::atomic<bool> releaseRoot{false};
    std::atomic<bool> innerDone{false};
    std::atomic<bool> releaseOuter{false};
    // The real root task has already constructed its ThreadsListHandle when
    // the first product root value reaches this existing result consumer.
    const auto consumeRoots = [&] {
        ZMark::VisitMinorRoots([&](BaseObject* object) {
            if (object == state.marker.load()) {
                rootObserved.store(true, std::memory_order_release);
                while (!releaseRoot.load(std::memory_order_acquire)) { std::this_thread::yield(); }
            }
        }, [](BaseObject*) {});
    };
    std::thread roots([&] {
        if (nested) {
            ThreadsListHandle outer;
            consumeRoots();
            innerDone.store(true, std::memory_order_release);
            while (!releaseOuter.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        } else {
            consumeRoots();
        }
    });
    const bool observed = WaitForSMR([&] { return rootObserved.load(std::memory_order_acquire); });
    bool removed = false;
    bool exitSettled = false;
    if (exitDuringRoots && observed) {
        state.exitTarget.store(true, std::memory_order_release);
        removed = WaitForSMR([&] {
            ThreadsListHandle current;
            return !current.includes(identity);
        });
        // Observe the real exit's wait, or its completed return on the cut
        // arm. No sleep window, injected product hook, or thread pointer read.
        exitSettled = WaitForSMR([&] {
            if (state.exited.load(std::memory_order_acquire)) { return true; }
            std::ifstream status("/proc/self/task/" + std::to_string(state.targetTid.load()) + "/wchan");
            std::string where;
            status >> where;
            return where.find("futex") != std::string::npos;
        });
    }
    bool retained = false;
    bool statsPreserved = !exitDuringRoots;
    size_t allocated = 0;
    // This is an independent product inventory, not the protecting handle.
    // A deleted Mutator has detached from it; never dereference its old pointer.
    ThreadGCData::VisitOwners([&](ThreadGCData&, Mutator* owner, ThreadLocalData*) {
        if (owner != identity) { return; }
        retained = true;
        if (exitDuringRoots) {
            allocated = owner->GetStackWatermark().stats().allocatedSize;
            statsPreserved = owner->GetStackWatermark().IsDone(epoch) && allocated > 0;
        }
    });
    std::fprintf(stderr, "THREAD_SNAPSHOT_LIFETIME_TARGET executed=1 exit=%d observed=%d removed=%d settled=%d retained=%d stats=%d allocated=%zu returned=%d\n",
                 exitDuringRoots, observed, removed, exitSettled, retained, statsPreserved, allocated, state.exited.load());
    // On a broken SO do not resume a consumer whose input object was reclaimed.
    // The explicit target assertion is the verdict; cleanup cannot hide it.
    try {
        GC_EXPECT_TRUE(observed && retained && statsPreserved &&
                       (!exitDuringRoots || (removed && exitSettled && !state.exited.load())));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "THREAD_SNAPSHOT_LIFETIME_ASSERT_FAIL %s\n", error.what());
        std::fflush(stderr);
        _exit(1);
    }
    releaseRoot.store(true, std::memory_order_release);
    if (nested) {
        const bool consumed = WaitForSMR([&] { return innerDone.load(std::memory_order_acquire); });
        // Give the exit thread a definite chance to report premature completion,
        // or observe it still sleeping in the product's deletion wait.
        const bool settled = WaitForSMR([&] {
            if (state.exited.load()) { return true; }
            std::ifstream status("/proc/self/task/" + std::to_string(state.targetTid.load()) + "/wchan");
            std::string where;
            status >> where;
            return where.find("futex") != std::string::npos;
        });
        bool outerRetained = false;
        ThreadGCData::VisitOwners([&](ThreadGCData&, Mutator* owner, ThreadLocalData*) {
            outerRetained |= owner == identity;
        });
        std::fprintf(stderr, "THREAD_SNAPSHOT_NESTED_TARGET executed=1 consumed=%d settled=%d retained=%d returned=%d\n",
                     consumed, settled, outerRetained, state.exited.load());
        try {
            GC_EXPECT_TRUE(consumed && settled && outerRetained && !state.exited.load());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "THREAD_SNAPSHOT_NESTED_ASSERT_FAIL %s\n", error.what());
            std::fflush(stderr);
            _exit(1);
        }
        releaseOuter.store(true, std::memory_order_release);
    }
    roots.join();
    state.finish.store(true, std::memory_order_release);
    state.exitTarget.store(true, std::memory_order_release);
    first.join();
    target.join();
    bool stillRegistered = false;
    ThreadGCData::VisitOwners([&](ThreadGCData&, Mutator* owner, ThreadLocalData*) {
        stillRegistered |= owner == identity;
    });
    std::fprintf(stderr, "THREAD_SNAPSHOT_RECLAIM_TARGET executed=1 returned=%d registered=%d\n",
                 state.exited.load(), stillRegistered);
    GC_EXPECT_TRUE(state.exited.load() && !stillRegistered);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
}
GC_RUNTIME_OTHER_VM_TEST(ThreadSnapshot, ExitBeforeYoungRootConsumption)
{
    CheckConcurrentRootSnapshot(true);
}
GC_RUNTIME_OTHER_VM_TEST(ThreadSnapshot, LiveYoungRootConsumption)
{
    CheckConcurrentRootSnapshot(false);
}
GC_RUNTIME_OTHER_VM_TEST(ThreadSnapshot, NestedHandleOutlivesYoungRootTask)
{
    CheckConcurrentRootSnapshot(true, true);
}
#endif

#if defined(__linux__)
namespace {
// Opaque product scheduler interfaces declared in inner/cjthread.h. Keep
// scheduler-private storage out of the test executable.
extern "C" struct CJThread* CJThreadBuild(ScheduleHandle, const CJThreadAttr*, CJThreadFunc,
                                         const void*, unsigned int, CJThreadCreateSource, uintptr_t);
extern "C" void CJ_CJThreadFree(struct CJThread*, bool);
struct CancelledThreadCase {
    bool created = false;
    bool registered = false;
    bool removed = false;
    CJThreadHandle cleanupTask = nullptr;
};
void* EmptySnapshotTask(void*) { return nullptr; }
void* NeverScheduledSnapshotTask(void*, unsigned int) { return nullptr; }
void* CancelBuiltSnapshotTask(void* value)
{
    auto& state = *static_cast<CancelledThreadCase*>(value);
    CJThreadAttr attr;
    CJThreadAttrInit(&attr);
    // Use the same product construction and recycling entries as the
    // enqueue-failure branch in CJThreadNew; no Mutator is manufactured here.
    auto* carrier = CJThreadBuild(reinterpret_cast<ScheduleHandle>(ThreadLocal::GetSchedule()), &attr, NeverScheduledSnapshotTask,
                                 nullptr, 0, CJTHREAD_CREATE_SOURCE_DEFAULT, ZPointerStoreGoodMask);
    state.created = carrier != nullptr;
    if (carrier == nullptr) { return nullptr; }
    Mutator* identity = nullptr;
    {
        ThreadsListHandle list;
        for (size_t i = 0; i < list.length(); ++i) {
            if (list.thread_at(i)->GetCjthreadPtr() == carrier) { identity = list.thread_at(i); }
        }
        state.registered = identity != nullptr;
    }
    CJ_CJThreadFree(carrier, true);
    {
        ThreadsListHandle list;
        state.removed = !list.includes(identity);
    }
    // Run a real task on the reusable carrier so the scheduling state also
    // follows its normal completion path before shutting down the runtime.
    state.cleanupTask = RunCJTask(EmptySnapshotTask, nullptr);
    return nullptr;
}
}
GC_RUNTIME_OTHER_VM_TEST(ThreadSnapshot, UnstartedCarrierReleasesIdentity)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    CancelledThreadCase state;
    auto task = RunCJTask(CancelBuiltSnapshotTask, &state);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &result), E_OK);
    ReleaseHandle(task);
    if (state.cleanupTask != nullptr) {
        GC_EXPECT_EQ(GetTaskRet(state.cleanupTask, &result), E_OK);
        ReleaseHandle(state.cleanupTask);
    }
    std::fprintf(stderr, "THREAD_SNAPSHOT_CANCEL_TARGET executed=1 created=%d registered=%d removed=%d\n",
                 state.created, state.registered, state.removed);
    GC_EXPECT_TRUE(state.created && state.registered && state.removed);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
#endif

#if defined(__linux__)
namespace {
struct TailMarkCase {
    TypeInfo* type;
    std::atomic<uintptr_t> object{0};
    std::atomic<uintptr_t> tail{0};
    std::atomic<uintptr_t> bound{0};
    std::atomic<bool> allocated{false};
    std::atomic<bool> retire{false};
    std::atomic<bool> retired{false};
    std::atomic<bool> release{false};
};
void TailMarkOwner(TailMarkCase& state)
{
    auto& manager = MutatorManager::Instance();
    Mutator* owner = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    owner->DoLeaveSaferegion();
    BaseObject* obj = reinterpret_cast<BaseObject*>(MCC_NewObject(state.type, 4096));
    AllocBuffer* buffer = AllocBuffer::GetAllocBuffer();
    uintptr_t words[3];
    std::memcpy(words, buffer, sizeof(words));
    state.object.store(reinterpret_cast<uintptr_t>(obj), std::memory_order_release);
    state.tail.store(words[0], std::memory_order_release);
    state.bound.store(words[1], std::memory_order_release);
    owner->DoEnterSaferegion();
    state.allocated.store(true, std::memory_order_release);
    while (!state.retire.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    // threadLocalAllocBuffer.cpp:130-158: the owner retires its own buffer
    // (ZGC zThreadLocalAllocBuffer.cpp:60-67: retire_for_thread reaches the
    // owning thread's TLAB). The tail span becomes a filler object.
    owner->DoLeaveSaferegion();
    AllocBuffer::GetAllocBuffer()->RetireTLAB(true);
    owner->DoEnterSaferegion();
    state.retired.store(true, std::memory_order_release);
    while (!state.release.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
}
}

// #826: the NW256 crash consumer chain. A slot naming the TLAB tail reaches
// the real mark consumer ZMark::MarkEntryObject (zMark.cpp:620-634; the crash
// instruction was GetSize+0x12 reading TypeInfo+8 on a zero head). After the
// retire above the tail is a filler with a valid header, so the same entry
// must complete. Red arm: revert the retire coverage and the tail stays raw,
// so this faults with the original signature (see the #822 report evidence).
GC_RUNTIME_OTHER_VM_TEST(TLABTail, RetiredTailSurvivesMarkEntry)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    TailMarkCase state{TLABTestType()};
    std::thread worker([&] { TailMarkOwner(state); });
    while (!state.allocated.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    auto& young = Heap::GetHeap().young();
    young.Workers()->set_active_workers(1);
    young.Begin(1);
    young.pause_mark_start();
    state.retire.store(true, std::memory_order_release);
    while (!state.retired.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    const uintptr_t tail = state.tail.load(std::memory_order_acquire);
    const uintptr_t bound = state.bound.load(std::memory_order_acquire);
    GC_EXPECT_TRUE(tail != 0 && bound > tail);
    BaseObject* tailObj = reinterpret_cast<BaseObject*>(tail);
    GC_EXPECT_TRUE(CollectedHeap::is_filler_object(tailObj));
    BaseObject* obj = reinterpret_cast<BaseObject*>(state.object.load(std::memory_order_acquire));
    // Positive control: the live object takes the same entry without faulting.
    GC_EXPECT_FALSE(ZMark::MarkEntryObject(obj,
        MarkStackEntry(untype(ZAddress::offset(from_object(obj))), true, true, false, false), nullptr));
    // Target: the retired tail filler through the same consumer.
    GC_EXPECT_FALSE(ZMark::MarkEntryObject(tailObj,
        MarkStackEntry(untype(ZAddress::offset(from_object(tailObj))), true, true, false, false), nullptr));
    std::fprintf(stderr, "TLAB_TAIL_TARGET executed=1 tail=%#zx bound=%#zx tail_size=%zu obj_size=%zu\n",
                 tail, bound, tailObj->GetSize(), obj->GetSize());
    state.release.store(true, std::memory_order_release);
    worker.join();
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
#endif
