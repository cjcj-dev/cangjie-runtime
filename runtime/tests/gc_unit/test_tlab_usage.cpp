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
GC_RUNTIME_OTHER_VM_TEST(TLABSnapshot, RootPublicationPreservesLaterRefills)
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
    heap.young().Workers()->set_active_workers(1);
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
