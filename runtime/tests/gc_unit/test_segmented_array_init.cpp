// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#if defined(MRT_GC_UNIT_TESTS)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__linux__)
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "gc_unittest.hpp"

#include "Cangjie.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/GcRequest.h"
#include "Heap/Heap.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/MArray.inline.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
extern "C" ArrayRef MCC_NewObjArray(const TypeInfo* arrayInfo, MIndex nElems);
extern "C" ArrayRef MCC_NewArray8(const TypeInfo* arrayInfo, MIndex nElems);
}

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
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

struct ByteArrayTypeInfos {
    ByteArrayTypeInfos()
    {
        std::memset(componentStorage, 0, sizeof(componentStorage));
        component = reinterpret_cast<TypeInfo*>(componentStorage);
        component->SetType(TypeKind::TYPE_KIND_UINT8);
        component->SetInstanceSize(sizeof(uint8_t));

        std::memset(arrayStorage, 0, sizeof(arrayStorage));
        array = reinterpret_cast<TypeInfo*>(arrayStorage);
        array->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        array->SetComponentTypeInfo(component);

        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(this), sizeof(*this));
    }

    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)];
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)];
    TypeInfo* component = nullptr;
    TypeInfo* array = nullptr;
};

ByteArrayTypeInfos& GetByteArrayTypeInfos()
{
    static ByteArrayTypeInfos infos;
    return infos;
}

enum class YieldGc : uint8_t {
    NONE,
    YOUNG,
    FULL,
};

enum class AllocationSource : uint8_t {
    INACTIVE,
    DIRTY,
    RELEASED,
    GARBAGE,
};

struct SegmentedArrayContext {
    static SegmentedArrayContext* current;

    explicit SegmentedArrayContext(MIndex arrayLength, YieldGc requestedGc = YieldGc::NONE)
        : length(arrayLength), gc(requestedGc)
    {
        LargeArrayInitTestHooks hooks;
        hooks.onPublish = OnPublish;
        hooks.onYield = OnYield;
        hooks.onWithdraw = OnWithdraw;
        hooks.onRootVisit = OnRootVisit;
        hooks.onRootPhase = OnRootPhase;
        current = this;
        CJ_MRT_SetLargeArrayInitTestHooks(&hooks);
    }

    ~SegmentedArrayContext()
    {
        CJ_MRT_SetLargeArrayInitTestHooks(nullptr);
        current = nullptr;
    }

    static void OnPublish(MArray* array)
    {
        ++current->publishCount;
        current->publishedArray = array;
        if (!array->IsInvisibleObject()) {
            ++current->failures;
        }
    }

    static bool ByteRangeEquals(const uint8_t* begin, size_t size, uint8_t expected)
    {
        for (size_t i = 0; i < size; ++i) {
            if (begin[i] != expected) {
                return false;
            }
        }
        return true;
    }

    static void OnYield(size_t segmentIndex)
    {
        SegmentedArrayContext& ctx = *current;
        ++ctx.yieldCount;
        if (segmentIndex == 0) {
            ++ctx.firstSegmentYieldCount;
        }

        MArray* rootBefore = static_cast<MArray*>(Mutator::GetMutator()->LoadInvisibleRoot());
        if (rootBefore == nullptr || rootBefore != ctx.publishedArray ||
            rootBefore->GetTypeInfo() != GetReferenceArrayTypeInfos().array ||
            rootBefore->GetLength() != ctx.length || !rootBefore->IsInvisibleObject()) {
            ++ctx.failures;
            return;
        }
        if (segmentIndex == 0) {
            const StateWord beforeLock = rootBefore->GetStateWord();
            if (!rootBefore->TryLockObject(beforeLock)) {
                ++ctx.failures;
            } else {
                if (!rootBefore->IsInvisibleObject()) {
                    ++ctx.failures;
                }
                rootBefore->UnlockObject(ObjectState::NORMAL);
                if (!rootBefore->IsInvisibleObject()) {
                    ++ctx.failures;
                }
            }
        }

        // Substantive allocator arm: exact dirty large-region reuse must expose
        // one zeroed segment while leaving the next segment dirty. A pre-loop
        // ClearUnits makes this suffix check fail at the first yield.
        if (segmentIndex == 0 && !ctx.checkedDirtyBoundary && ctx.dirtyAddress != 0) {
            ctx.checkedDirtyBoundary = true;
            const uint8_t* content = rootBefore->ConvertToCArray();
            const size_t segment = MArray::LARGE_REF_ARRAY_INIT_SEGMENT_SIZE;
            if (reinterpret_cast<uintptr_t>(rootBefore) != ctx.dirtyAddress ||
                !ByteRangeEquals(content, segment, 0) ||
                !ByteRangeEquals(content + segment, segment, ctx.dirtyByte)) {
                ++ctx.failures;
            }
        }

        if (!ctx.requestedGc && ctx.gc != YieldGc::NONE && segmentIndex == 0) {
            ctx.requestedGc = true;
            ctx.gcCountBefore = g_gcCount.load(std::memory_order_acquire);
            Mutator* mutator = Mutator::GetMutator();
            ctx.requestingMutator = mutator;
            U64 youngSeedRoot = 0;
            if (ctx.gc == YieldGc::YOUNG) {
                // Large arrays are old-generation regions. Plant one genuine
                // young allocation so this request cannot take the empty-young
                // fast path before VisitMinorRootSlots/FixMinorRootSlots.
                ScopedObjectAccess access;
                MArray* youngSeed = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, 16);
                if (youngSeed == nullptr) {
                    ++ctx.failures;
                } else {
                    youngSeedRoot = Heap::GetHeap().RegisterExportRoot(youngSeed);
                }
            }
            mutator->SetManagedContext(false);
            if (ctx.gc == YieldGc::YOUNG) {
                Heap::GetHeap().GetCollector().RequestGC(GC_REASON_YOUNG, false);
            } else {
                Heap::GetHeap().GetCollector().RequestGC(GC_REASON_FORCE, false);
            }
            mutator->SetManagedContext(true);
            if (youngSeedRoot != 0) {
                Heap::GetHeap().RemoveExportObject(youngSeedRoot);
            }
            ctx.gcCountAfter = g_gcCount.load(std::memory_order_acquire);
            MArray* rootAfter = static_cast<MArray*>(Mutator::GetMutator()->LoadInvisibleRoot());
            if (ctx.gcCountAfter <= ctx.gcCountBefore || rootAfter == nullptr ||
                rootAfter->GetTypeInfo() != GetReferenceArrayTypeInfos().array ||
                rootAfter->GetLength() != ctx.length) {
                ++ctx.failures;
            }
            ctx.rootMoved = rootAfter != rootBefore;
            ctx.publishedArray = rootAfter;
        }
    }

    static void OnWithdraw(MArray* array)
    {
        ++current->withdrawCount;
        current->withdrawnArray = array;
        if (array->IsInvisibleObject()) {
            ++current->failures;
        }
    }

    static void OnRootVisit(LargeArrayRootVisitSite site, BaseObject* object)
    {
        if (current->publishedArray != nullptr && object == current->publishedArray) {
            current->rootVisitSites |= static_cast<uint32_t>(1U << static_cast<unsigned>(site));
        }
    }

    static void OnRootPhase(LargeArrayRootPhase phase, Mutator* mutator, bool watermarkDone)
    {
        if (current->requestingMutator != mutator) {
            return;
        }
        if (phase == LargeArrayRootPhase::MAJOR_MARK) {
            ++current->majorRootPhaseObservations;
            current->majorWatermarkDone = watermarkDone;
        } else {
            ++current->minorRootPhaseObservations;
            current->minorWatermarkDone = watermarkDone;
        }
    }

    MIndex length;
    YieldGc gc;
    uintptr_t dirtyAddress = 0;
    uint8_t dirtyByte = 0xa5;
    size_t publishCount = 0;
    size_t yieldCount = 0;
    size_t firstSegmentYieldCount = 0;
    size_t withdrawCount = 0;
    size_t failures = 0;
    size_t gcCountBefore = 0;
    size_t gcCountAfter = 0;
    bool checkedDirtyBoundary = false;
    bool requestedGc = false;
    bool rootMoved = false;
    size_t majorRootPhaseObservations = 0;
    size_t minorRootPhaseObservations = 0;
    bool majorWatermarkDone = false;
    bool minorWatermarkDone = false;
    uint32_t rootVisitSites = 0;
    Mutator* requestingMutator = nullptr;
    MArray* publishedArray = nullptr;
    MArray* withdrawnArray = nullptr;
};

SegmentedArrayContext* SegmentedArrayContext::current = nullptr;

bool AllSlotsAreRawNull(MArray* array)
{
    bool allNull = true;
    array->ForEachRefField([&](RefField<>& slot) {
        if (!is_null(slot.GetFieldValue())) {
            allNull = false;
        }
    });
    return allNull;
}

constexpr uint32_t RootVisitBit(LargeArrayRootVisitSite site)
{
    return uint32_t { 1 } << static_cast<unsigned>(site);
}

uint32_t RequiredPhaseRootVisits(YieldGc gc, bool watermarkDone)
{
    // The epoch handshake has two legitimate root-production paths. A completed
    // watermark owns the root visit; otherwise the closing STW falls back to the
    // mutator walk. Young mark wraps that fallback with MINOR_MARK. Keep the
    // condition in the assertion: MINOR_MARK is not required after watermark DONE.
    if (watermarkDone) {
        return RootVisitBit(LargeArrayRootVisitSite::STACK_WATERMARK_NATIVE);
    }
    uint32_t required = RootVisitBit(LargeArrayRootVisitSite::MUTATOR_STACK_NATIVE);
    if (gc == YieldGc::YOUNG) {
        required |= RootVisitBit(LargeArrayRootVisitSite::MINOR_MARK);
    }
    return required;
}

constexpr MIndex kLargeRefLength = static_cast<MIndex>(
    (MArray::LARGE_REF_ARRAY_INIT_SEGMENT_SIZE * 2) / sizeof(void*) + 1);

bool PrepareExactLargeExtent(AllocationSource source, SegmentedArrayContext& ctx)
{
    RegionManager& manager =
        reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    const MIndex arraySize = CalculateArraySize(kLargeRefLength, RefField<>::GetSize());
    const size_t unitCount =
        (static_cast<size_t>(arraySize) + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;

    if (source == AllocationSource::INACTIVE) {
        ctx.dirtyAddress = manager.GetInactiveZone();
        return true;
    }

    RegionInfo* prepared = manager.TakeRegion(
        unitCount, RegionInfo::UnitRole::LARGE_SIZED_UNITS, false, true, true);
    if (prepared == nullptr) {
        return false;
    }
    ctx.dirtyAddress = prepared->GetRegionStart();
    switch (source) {
        case AllocationSource::DIRTY:
            manager.ReclaimRegion(prepared);
            break;
        case AllocationSource::RELEASED:
            (void)manager.ReleaseRegion(prepared);
            break;
        case AllocationSource::GARBAGE:
            (void)manager.CollectRegion<Generation::Old>(prepared);
            break;
        case AllocationSource::INACTIVE:
            return false;
    }
    if (source == AllocationSource::DIRTY || source == AllocationSource::GARBAGE) {
        std::memset(reinterpret_cast<void*>(ctx.dirtyAddress), ctx.dirtyByte,
                    unitCount * RegionInfo::UNIT_SIZE);
    }
    return true;
}

void* RunAllocationSourceCase(void* rawSource)
{
    const uintptr_t mode = reinterpret_cast<uintptr_t>(rawSource);
    const AllocationSource source = static_cast<AllocationSource>(mode & 0xffU);
    const bool nativeContext = (mode & 0x100U) != 0;
    SegmentedArrayContext ctx(kLargeRefLength);
    if (!PrepareExactLargeExtent(source, ctx)) {
        return reinterpret_cast<void*>(1);
    }
    const uintptr_t expectedAddress = ctx.dirtyAddress;
    if (source == AllocationSource::INACTIVE || source == AllocationSource::RELEASED) {
        ctx.dirtyAddress = 0;
    }

    Mutator* mutator = Mutator::GetMutator();
    if (nativeContext) {
        mutator->SetManagedContext(false);
    }
    MArray* array = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    if (nativeContext) {
        mutator->SetManagedContext(true);
    }
    size_t status = ctx.failures;
    status += array == nullptr ? 1 : 0;
    status += reinterpret_cast<uintptr_t>(array) == expectedAddress ? 0 : 1;
    status += ctx.publishCount == 1 ? 0 : 1;
    status += ctx.yieldCount >= 2 ? 0 : 1;
    status += ctx.withdrawCount == 1 ? 0 : 1;
    status += array != nullptr && AllSlotsAreRawNull(array) ? 0 : 1;
    if (source == AllocationSource::DIRTY || source == AllocationSource::GARBAGE) {
        status += ctx.checkedDirtyBoundary ? 0 : 1;
    }
    std::fprintf(stderr,
                 "[SEGMENTED_ARRAY_SOURCE] context=%s source=%u status=%zu expected=%#lx array=%p "
                 "publish=%zu yield=%zu withdraw=%zu dirty=%d null=%d\n",
                 nativeContext ? "native" : "managed", static_cast<unsigned>(source), status,
                 static_cast<unsigned long>(expectedAddress),
                 static_cast<void*>(array), ctx.publishCount, ctx.yieldCount, ctx.withdrawCount,
                 ctx.checkedDirtyBoundary, array != nullptr && AllSlotsAreRawNull(array));
    std::fflush(stderr);
    return reinterpret_cast<void*>(status);
}

void* RunSegmentedCase(void* rawMode)
{
    const YieldGc gc = static_cast<YieldGc>(reinterpret_cast<uintptr_t>(rawMode));
    SegmentedArrayContext ctx(kLargeRefLength, gc);
    // Install the exact dirty extent directly. Allocating a byte array and then
    // hoping that a full collection returns the same address is not invariant:
    // a two-processor runtime may consume the reclaimed extent first.
    if (!PrepareExactLargeExtent(AllocationSource::DIRTY, ctx)) {
        return reinterpret_cast<void*>(1);
    }

    MArray* array = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    size_t status = ctx.failures;
    status += array == nullptr ? 1 : 0;
    status += ctx.publishCount == 1 ? 0 : 1;
    status += ctx.yieldCount >= 2 ? 0 : 1;
    status += ctx.checkedDirtyBoundary ? 0 : 1;
    status += ctx.withdrawCount == 1 ? 0 : 1;
    status += array != nullptr && ctx.withdrawnArray == array ? 0 : 1;
    status += array != nullptr && !array->IsInvisibleObject() ? 0 : 1;
    status += Mutator::GetMutator()->LoadInvisibleRoot() == nullptr ? 0 : 1;
    status += array != nullptr && AllSlotsAreRawNull(array) ? 0 : 1;
    if (gc != YieldGc::NONE) {
        status += ctx.requestedGc ? 0 : 1;
        status += ctx.gcCountAfter > ctx.gcCountBefore ? 0 : 1;
        status += ctx.firstSegmentYieldCount >= 2 ? 0 : 1;
        const bool phaseObserved = gc == YieldGc::YOUNG
            ? ctx.minorRootPhaseObservations == 1
            : ctx.majorRootPhaseObservations == 1;
        const bool watermarkDone = gc == YieldGc::YOUNG
            ? ctx.minorWatermarkDone
            : ctx.majorWatermarkDone;
        status += phaseObserved ? 0 : 1;
        uint32_t required = RequiredPhaseRootVisits(gc, watermarkDone) |
            RootVisitBit(LargeArrayRootVisitSite::ITERATOR_SKIP);
        if (gc == YieldGc::YOUNG) {
            // Relocation's grant pass independently enumerates the native side
            // root before MINOR_RELOCATE consumes it. This remains required even
            // when mark used the completed watermark path.
            required |= RootVisitBit(LargeArrayRootVisitSite::MUTATOR_STACK_NATIVE) |
                RootVisitBit(LargeArrayRootVisitSite::MINOR_RELOCATE);
        }
        status += (ctx.rootVisitSites & required) == required ? 0 : 1;
    }
    std::fprintf(stderr,
                 "[SEGMENTED_ARRAY_CASE] mode=%u status=%zu failures=%zu dirty=%d dirty_addr=%#lx "
                 "array=%p publish=%zu yield=%zu first=%zu withdraw=%zu requested_gc=%d "
                 "gc_before=%zu gc_after=%zu moved=%d root_sites=%#x phase_n=%zu watermark_done=%d null=%d\n",
                 static_cast<unsigned>(gc), status, ctx.failures, ctx.checkedDirtyBoundary,
                 static_cast<unsigned long>(ctx.dirtyAddress), static_cast<void*>(array), ctx.publishCount,
                 ctx.yieldCount, ctx.firstSegmentYieldCount, ctx.withdrawCount, ctx.requestedGc,
                 ctx.gcCountBefore, ctx.gcCountAfter, ctx.rootMoved, ctx.rootVisitSites,
                 gc == YieldGc::YOUNG ? ctx.minorRootPhaseObservations : ctx.majorRootPhaseObservations,
                 gc == YieldGc::YOUNG ? ctx.minorWatermarkDone : ctx.majorWatermarkDone,
                 array != nullptr && AllSlotsAreRawNull(array));
    std::fflush(stderr);
    return reinterpret_cast<void*>(status);
}

void* RunSmallReferenceCase(void* rawNative)
{
    constexpr MIndex length = 16;
    SegmentedArrayContext ctx(length);
    Mutator* mutator = Mutator::GetMutator();
    const bool nativeContext = reinterpret_cast<uintptr_t>(rawNative) != 0;
    if (nativeContext) {
        mutator->SetManagedContext(false);
    }
    MArray* array = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, length);
    if (nativeContext) {
        mutator->SetManagedContext(true);
    }
    size_t status = 0;
    status += array == nullptr ? 1 : 0;
    status += ctx.publishCount == 0 ? 0 : 1;
    status += ctx.yieldCount == 0 ? 0 : 1;
    status += ctx.withdrawCount == 0 ? 0 : 1;
    status += array != nullptr && AllSlotsAreRawNull(array) ? 0 : 1;
    return reinterpret_cast<void*>(status);
}

void* RunLargePrimitiveCase(void* rawNative)
{
    constexpr MIndex length = static_cast<MIndex>(MArray::LARGE_REF_ARRAY_INIT_SEGMENT_SIZE * 2 + 1);
    SegmentedArrayContext ctx(length);
    Mutator* mutator = Mutator::GetMutator();
    const bool nativeContext = reinterpret_cast<uintptr_t>(rawNative) != 0;
    if (nativeContext) {
        mutator->SetManagedContext(false);
    }
    MArray* array = MCC_NewArray8(GetByteArrayTypeInfos().array, length);
    if (nativeContext) {
        mutator->SetManagedContext(true);
    }
    size_t status = 0;
    status += array == nullptr ? 1 : 0;
    status += ctx.publishCount == 0 ? 0 : 1;
    status += ctx.yieldCount == 0 ? 0 : 1;
    status += ctx.withdrawCount == 0 ? 0 : 1;
    return reinterpret_cast<void*>(status);
}

int RunRuntimeCase(CJTaskFunc task, uintptr_t argument, U32 processorCount = 1)
{
#if defined(__linux__)
    const pid_t child = fork();
    if (child == 0) {
        (void)setenv("cjProcessorNum", processorCount == 1 ? "1" : "2", 1);
        RuntimeParam param {};
        param.heapParam.heapSize = 32 * 1024;
        param.coParam.processorNum = processorCount;
        if (InitCJRuntime(&param) != E_OK) {
            _exit(100);
        }
        CJThreadHandle handle = RunCJTask(task, reinterpret_cast<void*>(argument));
        if (handle == nullptr) {
            _exit(101);
        }
        void* result = nullptr;
        if (GetTaskRet(handle, &result) != E_OK) {
            _exit(102);
        }
        ReleaseHandle(handle);
        const uintptr_t status = reinterpret_cast<uintptr_t>(result);
        if (FiniCJRuntime() != E_OK) {
            _exit(103);
        }
        _exit(status > 99 ? 99 : static_cast<int>(status));
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return 120;
    }
    return WEXITSTATUS(status);
#else
    (void)task;
    (void)argument;
    (void)processorCount;
    return 0;
#endif
}
} // namespace

GC_OTHER_VM_TEST(SegmentedArrayInit, YieldKeepsInvisibleRootAndPublishesBoundary)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunSegmentedCase, static_cast<uintptr_t>(YieldGc::NONE)), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, ManagedFirstInactiveExtentUsesSegmentedInitializer)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunAllocationSourceCase,
                                static_cast<uintptr_t>(AllocationSource::INACTIVE)), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, ManagedDirtyExtentUsesSegmentedInitializer)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunAllocationSourceCase,
                                static_cast<uintptr_t>(AllocationSource::DIRTY)), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, ManagedReleasedExtentUsesSegmentedInitializer)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunAllocationSourceCase,
                                static_cast<uintptr_t>(AllocationSource::RELEASED)), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, ManagedGarbageExtentUsesSegmentedInitializer)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunAllocationSourceCase,
                                static_cast<uintptr_t>(AllocationSource::GARBAGE)), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, NativeFirstInactiveExtentUsesSegmentedInitializer)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunAllocationSourceCase,
                                0x100U | static_cast<uintptr_t>(AllocationSource::INACTIVE)), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, NativeDirtyExtentUsesSegmentedInitializer)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunAllocationSourceCase,
                                0x100U | static_cast<uintptr_t>(AllocationSource::DIRTY)), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, NativeReleasedExtentUsesSegmentedInitializer)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunAllocationSourceCase,
                                0x100U | static_cast<uintptr_t>(AllocationSource::RELEASED)), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, NativeGarbageExtentUsesSegmentedInitializer)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunAllocationSourceCase,
                                0x100U | static_cast<uintptr_t>(AllocationSource::GARBAGE)), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, EpochFlipRestartsAndRewritesPublishedBlock)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunSegmentedCase, static_cast<uintptr_t>(YieldGc::FULL)), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, EpochFlipRestartsAndRewritesPublishedBlockParallel)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunSegmentedCase, static_cast<uintptr_t>(YieldGc::FULL), 2), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, YoungGcRepairsIncompleteArrayRoot)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunSegmentedCase, static_cast<uintptr_t>(YieldGc::YOUNG)), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, YoungGcRepairsIncompleteArrayRootParallel)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunSegmentedCase, static_cast<uintptr_t>(YieldGc::YOUNG), 2), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, SmallReferenceArrayKeepsFastPath)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunSmallReferenceCase, 0), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, NativeSmallReferenceArrayKeepsFastPath)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunSmallReferenceCase, 1), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, LargePrimitiveArrayKeepsFastPath)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunLargePrimitiveCase, 0), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, NativeLargePrimitiveArrayKeepsFastPath)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunLargePrimitiveCase, 1), 0);
}

#endif // MRT_GC_UNIT_TESTS
