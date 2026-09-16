// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#if defined(MRT_GC_UNIT_TESTS)

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <vector>
#if defined(__linux__)
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "gc_unittest.hpp"

#include "Cangjie.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/Collector/GcRequest.h"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zIterator.hpp"
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/WCollector/WCollector.h"

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

    explicit SegmentedArrayContext(MIndex arrayLength, YieldGc requestedGc = YieldGc::NONE,
                                   bool requireWatermarkDone = false)
        : length(arrayLength), gc(requestedGc), requireWatermarkDone(requireWatermarkDone)
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
            rootBefore->GetTypeInfo() != ctx.expectedType ||
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

            // zIterator.inline.hpp:64-70: incomplete reference arrays may
            // only enter safe iteration. These are product instantiations,
            // not a second implementation compiled into this test executable.
            RefFieldVisitor first = [&ctx](RefField<>&) { ++ctx.safeIteratorVisits[0]; };
            RefFieldVisitor second = [&ctx](RefField<>&) { ++ctx.safeIteratorVisits[1]; };
            ZBasicOopIterateClosure<RefFieldVisitor> firstClosure(first);
            ZBasicOopIterateClosure<RefFieldVisitor> secondClosure(second);
            ZIterator::oop_iterate_safe(rootBefore, &firstClosure);
            ZIterator::oop_iterate_safe(rootBefore, rootBefore->GetTypeInfo(), &secondClosure);
            ZIterator::basic_oop_iterate_safe(rootBefore,
                RefFieldVisitor([&ctx](RefField<>&) { ++ctx.safeIteratorVisits[2]; }));
            ZIterator::basic_oop_iterate_safe(rootBefore, rootBefore->GetTypeInfo(),
                RefFieldVisitor([&ctx](RefField<>&) { ++ctx.safeIteratorVisits[3]; }));
            const bool opaque = std::all_of(ctx.safeIteratorVisits.begin(), ctx.safeIteratorVisits.end(),
                                            [](size_t count) { return count == 0; });
            std::fprintf(stderr, "[SEGMENTED_ITERATOR_ASSERT] safe=%zu safe_klass=%zu basic=%zu basic_klass=%zu pass=%d\n",
                         ctx.safeIteratorVisits[0], ctx.safeIteratorVisits[1], ctx.safeIteratorVisits[2],
                         ctx.safeIteratorVisits[3], opaque);
            if (!opaque) {
                ++ctx.failures;
            }
        }

        // Substantive allocator arm: exact dirty large-region reuse must expose
        // one zeroed segment while leaving the next segment dirty. A pre-loop
        // ClearUnits makes this suffix check fail at the first yield.
        if (segmentIndex == 0 && !ctx.checkedDirtyBoundary && ctx.dirtyAddress != 0) {
            ctx.checkedDirtyBoundary = true;
            const uint8_t* content = rootBefore->ConvertToCArray();
            const size_t segment = MArray::LARGE_ARRAY_INIT_SEGMENT_SIZE;
            if (reinterpret_cast<uintptr_t>(rootBefore) != ctx.dirtyAddress ||
                !ByteRangeEquals(content, segment, 0) ||
                !ByteRangeEquals(content + segment, segment, ctx.dirtyByte)) {
                ++ctx.failures;
            }
        }

        if (ctx.gcRequests < ctx.gcLimit && ctx.gc != YieldGc::NONE) {
            ctx.requestedGc = true;
            ++ctx.gcRequests;
            ctx.youngSequenceBefore = Heap::GetHeap().GetCollector().GetCycleSnapshot(
                GCCycleGeneration::YOUNG).sequence;
            ctx.oldSequenceBefore = Heap::GetHeap().GetCollector().GetCycleSnapshot(
                GCCycleGeneration::OLD).sequence;
            ctx.colorBefore = ::g_cjStoreGoodMask;
            Mutator* mutator = Mutator::GetMutator();
            ctx.requestingMutator = mutator;
            U64 youngSeedRoot = 0;
            if (ctx.gc == YieldGc::YOUNG) {
                // Also plant a small young allocation so this request cannot take the empty-young
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
            ctx.youngSequenceAfter = Heap::GetHeap().GetCollector().GetCycleSnapshot(
                GCCycleGeneration::YOUNG).sequence;
            ctx.oldSequenceAfter = Heap::GetHeap().GetCollector().GetCycleSnapshot(
                GCCycleGeneration::OLD).sequence;
            ctx.colorAfter = ::g_cjStoreGoodMask;
            MArray* rootAfter = static_cast<MArray*>(Mutator::GetMutator()->LoadInvisibleRoot());
            if (!ctx.GcSafepointHappened() || rootAfter == nullptr ||
                rootAfter->GetTypeInfo() != ctx.expectedType ||
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
        if (array->GetComponentTypeInfo()->IsRef()) {
            // Positive control through the same product entry after publication
            // completes. Consume the product's continuations, checking every
            // slot address exactly once, without interpreting reference values.
            std::vector<unsigned char> visits(array->GetLength(), 0);
            std::vector<MarkStackEntry> pending;
            const MAddress start = reinterpret_cast<MAddress>(array->ConvertToCArray());
            size_t invalid = 0;
            auto visit = [&](MAddress field) {
                if (field < start || (field - start) % sizeof(RefField<>) != 0 ||
                    (field - start) / sizeof(RefField<>) >= visits.size()) {
                    ++invalid;
                } else {
                    ++visits[(field - start) / sizeof(RefField<>)];
                }
            };
            auto publish = [&](const MarkStackEntry& entry) { pending.push_back(entry); };
            MarkPartialArray::FollowObjectReferences(array, false, visit, publish);
            const size_t partials = pending.size();
            while (!pending.empty()) {
                const MarkStackEntry entry = pending.back();
                pending.pop_back();
                MarkPartialArray::FollowPartialReferences(entry, visit, publish);
            }
            for (unsigned char count : visits) {
                invalid += count != 1;
            }
            size_t full = 0;
            size_t basic = 0;
            size_t range = 0;
            RefFieldVisitor fullVisitor = [&](RefField<>&) { ++full; };
            RefFieldVisitor rangeVisitor = [&](RefField<>&) { ++range; };
            ZBasicOopIterateClosure<RefFieldVisitor> fullClosure(fullVisitor);
            ZBasicOopIterateClosure<RefFieldVisitor> rangeClosure(rangeVisitor);
            ZIterator::oop_iterate(array, &fullClosure);
            ZIterator::basic_oop_iterate(array, RefFieldVisitor([&](RefField<>&) { ++basic; }));
            ZIterator::oop_iterate_elements_range(array, &rangeClosure, 0, 1);
            std::array<size_t, 4> safe {};
            RefFieldVisitor first = [&](RefField<>&) { ++safe[0]; };
            RefFieldVisitor second = [&](RefField<>&) { ++safe[1]; };
            ZBasicOopIterateClosure<RefFieldVisitor> firstClosure(first);
            ZBasicOopIterateClosure<RefFieldVisitor> secondClosure(second);
            ZIterator::oop_iterate_safe(array, &firstClosure);
            ZIterator::oop_iterate_safe(array, array->GetTypeInfo(), &secondClosure);
            ZIterator::basic_oop_iterate_safe(array, RefFieldVisitor([&](RefField<>&) { ++safe[2]; }));
            ZIterator::basic_oop_iterate_safe(array, array->GetTypeInfo(),
                                             RefFieldVisitor([&](RefField<>&) { ++safe[3]; }));
            const bool complete = invalid == 0 && full == visits.size() && basic == visits.size() && range == 1 &&
                std::all_of(safe.begin(), safe.end(), [&](size_t count) { return count == visits.size(); });
            std::fprintf(stderr, "[SEGMENTED_VISIBLE_ASSERT] fields=%zu full=%zu basic=%zu range=%zu partial=%zu "
                         "safe=%zu safe_klass=%zu safe_basic=%zu safe_basic_klass=%zu invalid=%zu pass=%d\n",
                         visits.size(), full, basic, range, partials, safe[0], safe[1], safe[2], safe[3], invalid, complete);
            current->failures += !complete;
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

    TypeInfo* expectedType = GetReferenceArrayTypeInfos().array;
    size_t gcLimit = 1;
    size_t gcRequests = 0;
    MIndex length;
    YieldGc gc;
    uintptr_t dirtyAddress = 0;
    uint8_t dirtyByte = 0xa5;
    size_t publishCount = 0;
    size_t yieldCount = 0;
    size_t firstSegmentYieldCount = 0;
    size_t withdrawCount = 0;
    size_t failures = 0;
    std::array<size_t, 4> safeIteratorVisits {};
    uint64_t youngSequenceBefore = 0;
    uint64_t youngSequenceAfter = 0;
    uint64_t oldSequenceBefore = 0;
    uint64_t oldSequenceAfter = 0;
    uintptr_t colorBefore = 0;
    uintptr_t colorAfter = 0;
    bool GcSafepointHappened() const
    {
        return youngSequenceBefore != youngSequenceAfter || oldSequenceBefore != oldSequenceAfter ||
            colorBefore != colorAfter;
    }
    bool checkedDirtyBoundary = false;
    bool requestedGc = false;
    bool rootMoved = false;
    bool requireWatermarkDone = false;
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
    (MArray::LARGE_ARRAY_INIT_SEGMENT_SIZE * 2) / sizeof(void*) + 1);

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
        unitCount, ZPageType::large, false, true, true);
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
        // ZMappedCache keeps its entry inside the last granule of a cached
        // range (zMappedCache.cpp:92-119); dirtying cached memory must stay
        // clear of it. The yield check reads the first two segments only, so
        // dirty those (plus the array header that precedes the payload).
        const size_t dirtyBytes = std::min(unitCount * RegionInfo::UNIT_SIZE,
                                           2 * static_cast<size_t>(MArray::LARGE_ARRAY_INIT_SEGMENT_SIZE) + 64);
        std::memset(reinterpret_cast<void*>(ctx.dirtyAddress), ctx.dirtyByte, dirtyBytes);
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
    const uintptr_t mode = reinterpret_cast<uintptr_t>(rawMode);
    const YieldGc gc = static_cast<YieldGc>(mode & 0xffU);
    const bool requireWatermarkDone = (mode & 0x100U) != 0;
    SegmentedArrayContext ctx(kLargeRefLength, gc, requireWatermarkDone);
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
        status += ctx.GcSafepointHappened() ? 0 : 1;
        status += ctx.firstSegmentYieldCount >= 2 ? 0 : 1;
        const bool phaseObserved = gc == YieldGc::YOUNG
            ? ctx.minorRootPhaseObservations == 1
            : ctx.majorRootPhaseObservations == 1;
        const bool watermarkDone = gc == YieldGc::YOUNG
            ? ctx.minorWatermarkDone
            : ctx.majorWatermarkDone;
        if (requireWatermarkDone) {
            // The residual-fallback contract is intentionally marked unreachable
            // for this product path: RunEpochHandshake and the closing consumer
            // share one epoch, and Mutator::DrainStackWatermark pairs TryBegin
            // with Finish before Mark.cpp reads IsDone(epoch). This observes the
            // product expression without rewriting its result in a callback.
            // If that pair is split or the consumer reads another epoch, this
            // contract becomes measurable again and this guard must be restored.
            status += watermarkDone ? 0 : 1;
        }
        status += phaseObserved ? 0 : 1;
        uint32_t required = RequiredPhaseRootVisits(gc, watermarkDone) |
            RootVisitBit(LargeArrayRootVisitSite::ITERATOR_SKIP);
        // ZStackWatermark::process_head (zStackWatermark.cpp:171-173) owns
        // invisible-root processing. The unified phase path removed the second
        // native stack walk from FixMinorRootSlots (b4b43df103); requiring its
        // MUTATOR_STACK_NATIVE/MINOR_RELOCATE observations tests a removed path.
        const bool rootVisitsComplete = (ctx.rootVisitSites & required) == required;
        status += rootVisitsComplete ? 0 : 1;
        std::fprintf(stderr, "[SEGMENTED_ROOT_ASSERT] required=%#x actual=%#x pass=%d\n",
                     required, ctx.rootVisitSites, rootVisitsComplete);
        const uint32_t forbidden = RootVisitBit(LargeArrayRootVisitSite::MUTATOR_STACK_MANAGED) |
            RootVisitBit(LargeArrayRootVisitSite::STACK_WATERMARK_MANAGED);
        status += (ctx.rootVisitSites & forbidden) == 0 ? 0 : 1;
    }
    std::fprintf(stderr,
                 "[SEGMENTED_ARRAY_CASE] mode=%u status=%zu failures=%zu dirty=%d dirty_addr=%#lx "
                 "array=%p publish=%zu yield=%zu first=%zu withdraw=%zu requested_gc=%d context=%s "
                 "iterator_safe=%zu iterator_safe_klass=%zu "
                 "young_before=%llu young_after=%llu old_before=%llu old_after=%llu moved=%d root_sites=%#x phase_n=%zu watermark_done=%d null=%d\n",
                 static_cast<unsigned>(gc), status, ctx.failures, ctx.checkedDirtyBoundary,
                 static_cast<unsigned long>(ctx.dirtyAddress), static_cast<void*>(array), ctx.publishCount,
                 ctx.yieldCount, ctx.firstSegmentYieldCount, ctx.withdrawCount, ctx.requestedGc,
                 requireWatermarkDone ? "native-watermark" : "native",
                 ctx.safeIteratorVisits[0], ctx.safeIteratorVisits[1],
                 static_cast<unsigned long long>(ctx.youngSequenceBefore),
                 static_cast<unsigned long long>(ctx.youngSequenceAfter),
                 static_cast<unsigned long long>(ctx.oldSequenceBefore),
                 static_cast<unsigned long long>(ctx.oldSequenceAfter), ctx.rootMoved, ctx.rootVisitSites,
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

// ZObjArrayAllocator::initialize: primitive segments cooperate without restarting.
void* RunLargePrimitiveCase(void* rawMode)
{
    constexpr MIndex length = static_cast<MIndex>(MArray::LARGE_ARRAY_INIT_SEGMENT_SIZE * 2 + 1);
    const uintptr_t mode = reinterpret_cast<uintptr_t>(rawMode);
    SegmentedArrayContext ctx(length, (mode & 2U) ? YieldGc::FULL : YieldGc::NONE);
    ctx.expectedType = GetByteArrayTypeInfos().array;
    ctx.gcLimit = 2;
    Mutator* mutator = Mutator::GetMutator();
    const bool nativeContext = (mode & 1U) != 0;
    if (nativeContext) {
        mutator->SetManagedContext(false);
    }
    MArray* array = MCC_NewArray8(ctx.expectedType, length);
    if (nativeContext) {
        mutator->SetManagedContext(true);
    }
    size_t status = ctx.failures;
    status += array == nullptr ? 1 : 0;
    status += ctx.publishCount == 1 ? 0 : 1;
    status += ctx.yieldCount == 3 ? 0 : 1;
    status += ctx.firstSegmentYieldCount == 1 ? 0 : 1;
    status += ctx.withdrawCount == 1 ? 0 : 1;
    status += ctx.withdrawnArray == array ? 0 : 1;
    status += array != nullptr && !array->IsInvisibleObject() ? 0 : 1;
    status += array != nullptr && SegmentedArrayContext::ByteRangeEquals(
        array->ConvertToCArray(), length, 0) ? 0 : 1;
    status += mutator->LoadInvisibleRoot() == nullptr ? 0 : 1;
    if (mode & 2U) {
        status += ctx.gcRequests == 2 ? 0 : 1;
    }
    return reinterpret_cast<void*>(status);
}

// Two real GC requests at successive yield points: only the first may restart.
void* RunTwoGcReferenceCase(void*)
{
    SegmentedArrayContext ctx(kLargeRefLength, YieldGc::FULL);
    ctx.gcLimit = 2;
    MArray* array = MCC_NewObjArray(ctx.expectedType, kLargeRefLength);
    size_t status = ctx.failures;
    status += ctx.gcRequests == 2 ? 0 : 1;
    status += ctx.firstSegmentYieldCount == 2 ? 0 : 1;
    status += ctx.yieldCount == 4 ? 0 : 1;
    status += ctx.publishCount == 1 && ctx.withdrawCount == 1 ? 0 : 1;
    status += array != nullptr && array == ctx.withdrawnArray &&
        !array->IsInvisibleObject() && AllSlotsAreRawNull(array) ? 0 : 1;
    status += Mutator::GetMutator()->LoadInvisibleRoot() == nullptr ? 0 : 1;
    std::fprintf(stderr, "[SEGMENTED_TWO_GC] requests=%zu first=%zu yields=%zu status=%zu\n",
                 ctx.gcRequests, ctx.firstSegmentYieldCount, ctx.yieldCount, status);
    return reinterpret_cast<void*>(status);
}

// ZPage::reset(age), zPage.cpp:103-108. Observe the product allocation at
// array publication, before the mutator can consume the descriptor.
struct LargePageIdentityObservation {
    inline static bool publishedYoung = false;
    inline static uint8_t publishedAge = 255;
    inline static size_t publications = 0;
    static void Published(MArray* array)
    {
        RegionInfo* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(array));
        publishedYoung = page->IsYoungRegion();
        publishedAge = page->GetYoungAge();
        ++publications;
    }
};

void* RunLargePageIdentityCase(void* rawNative)
{
    LargePageIdentityObservation::publications = 0;
    LargeArrayInitTestHooks hooks;
    hooks.onPublish = LargePageIdentityObservation::Published;
    CJ_MRT_SetLargeArrayInitTestHooks(&hooks);
    Mutator* mutator = Mutator::GetMutator();
    const bool native = reinterpret_cast<uintptr_t>(rawNative) != 0;
    if (native) mutator->SetManagedContext(false);
    MArray* array = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    if (native) mutator->SetManagedContext(true);
    CJ_MRT_SetLargeArrayInitTestHooks(nullptr);
    if (array == nullptr || LargePageIdentityObservation::publications != 1) {
        std::fprintf(stderr, "LARGE_PAGE_IDENTITY_SETUP_FAILED publications=%zu\n",
                     LargePageIdentityObservation::publications);
        return reinterpret_cast<void*>(2);
    }
    RegionInfo* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(array));
    const bool young = LargePageIdentityObservation::publishedYoung;
    const uint8_t age = LargePageIdentityObservation::publishedAge;
    std::fprintf(stderr, "LARGE_PAGE_YOUNG_ASSERT_EXECUTED native=%d published_young=%d "
                 "published_age=%u final_young=%d large=%d\n", native, young, age,
                 page->IsYoungRegion(), page->IsLargeRegion());
    return reinterpret_cast<void*>((young && age == 0 && page->IsYoungRegion() &&
                                    page->IsLargeRegion()) ? 0 : 1);
}

#if defined(MRT_TESTABLE_INTERNALS)
struct LargeYoungClosureResult {
    inline static BaseObject* target = nullptr;
    inline static bool live = false;
    inline static bool followed = false;
    inline static size_t observations = 0;
    static void Observe(const std::vector<BaseObject*>* objects)
    {
        if (objects == nullptr || target == nullptr) return;
        RegionInfo* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(target));
        ++observations;
        live |= page->is_object_strongly_live(from_object(target));
        for (BaseObject* object : *objects) followed |= object == target;
    }
};

void* RunLargeYoungClosureCase(void*)
{
    // Construct the sole strong path before requesting GC. No unregistered
    // C++ reference is used to publish a target after marking has begun.
    MArray* holder = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    MArray* target = MCC_NewArray8(GetByteArrayTypeInfos().array, 16);
    auto& field = HeapSlotAt<>(reinterpret_cast<uintptr_t>(holder->ConvertToCArray()));
    Heap::GetBarrier().WriteReference(holder, field, target);
    const bool holderYoung = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(holder))->IsYoungRegion();
    const U64 holderRoot = Heap::GetHeap().RegisterExportRoot(holder);
    LargeYoungClosureResult::target = target;
    LargeYoungClosureResult::live = false;
    LargeYoungClosureResult::followed = false;
    LargeYoungClosureResult::observations = 0;
    SetMarkClosureObserverForTest(LargeYoungClosureResult::Observe);
    Mutator* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    Heap::GetHeap().GetCollector().RequestGC(GC_REASON_YOUNG, false);
    SetMarkClosureObserverForTest(nullptr);
    LargeYoungClosureResult::target = nullptr;
    std::fprintf(stderr, "LARGE_YOUNG_TARGET_LIVE_ASSERT_EXECUTED holder_young=%d observations=%zu live=%d followed=%d\n",
                 holderYoung, LargeYoungClosureResult::observations, LargeYoungClosureResult::live,
                 LargeYoungClosureResult::followed);
    Heap::GetHeap().RemoveExportObject(holderRoot);
    mutator->SetManagedContext(true);
    return reinterpret_cast<void*>((holderYoung && LargeYoungClosureResult::live && LargeYoungClosureResult::followed) ? 0 : 1);
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
// The observer stops the real concurrent collector after its first closure.
// The managed task allocates through MCC_NewObjArray while that TRACE window
// is held open; no fixture changes a page generation, mark bit, or phase.
struct MarkAllocationWindow {
    inline static std::atomic<bool> entered{false};
    inline static std::atomic<bool> released{false};
    inline static std::atomic<bool> completed{false};
    inline static bool timedOut = false;
    static bool Wait(const std::atomic<bool>& flag)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return true;
    }
    static void Observe(const std::vector<BaseObject*>* objects)
    {
        if (objects == nullptr || entered.exchange(true)) return;
        timedOut = !Wait(released);
        completed.store(true, std::memory_order_release);
    }
};

void* RunMarkAllocationCase(void* rawExisting)
{
    const bool existing = reinterpret_cast<uintptr_t>(rawExisting) != 0;
    // ZPage::is_object_strongly_live (zPage.inline.hpp:258-260): the page
    // predicate over the product-owned livemap.
    auto productLive = [](RegionInfo* page, const BaseObject* object) {
        return page->is_object_strongly_live(from_object(object));
    };
    auto& heap = Heap::GetHeap();
    auto& collector = heap.GetCollector();
    Mutator* mutator = Mutator::GetMutator();
    MArray* beforeSmall = MCC_NewArray8(GetByteArrayTypeInfos().array, 16);
    const U64 beforeSmallRoot = heap.RegisterExportRoot(beforeSmall);
    RegionInfo* beforeSmallPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(beforeSmall));
    MArray* target = existing ? MCC_NewArray8(GetByteArrayTypeInfos().array, 16) : nullptr;
    const U64 targetRoot = target != nullptr ? heap.RegisterExportRoot(target) : 0;
    MarkAllocationWindow::entered = false;
    MarkAllocationWindow::released = false;
    MarkAllocationWindow::completed = false;
    MarkAllocationWindow::timedOut = false;
    SetMarkClosureObserverForTest(MarkAllocationWindow::Observe);
    mutator->SetManagedContext(false);
    collector.RequestGC(GC_REASON_YOUNG, true);
    bool entered;
    {
        ScopedEnterSaferegion safe(false);
        entered = MarkAllocationWindow::Wait(MarkAllocationWindow::entered);
    }
    if (!entered) {
        MarkAllocationWindow::released = true;
        SetMarkClosureObserverForTest(nullptr);
        mutator->SetManagedContext(true);
        return reinterpret_cast<void*>(2);
    }
    mutator->SetManagedContext(true);
    MArray* afterSmall = MCC_NewArray8(GetByteArrayTypeInfos().array, 16);
    RegionInfo* afterSmallPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(afterSmall));
    const bool retiredTLAB = afterSmallPage != beforeSmallPage && afterSmallPage->IsAllocating();
    std::fprintf(stderr, "P1_TLAB_RETIRE_ASSERT_EXECUTED different=%d birth=%llu owner=%llu\n",
                 afterSmallPage != beforeSmallPage,
                 static_cast<unsigned long long>(afterSmallPage->BirthSequence()),
                 static_cast<unsigned long long>(afterSmallPage->GetSnapshotEpoch()));
    MArray* holder = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    if (!existing) target = MCC_NewArray8(GetByteArrayTypeInfos().array, 16);
    const U64 holderRoot = heap.RegisterExportRoot(holder);
    auto& field = HeapSlotAt<>(reinterpret_cast<uintptr_t>(holder->ConvertToCArray()));
    Heap::GetBarrier().WriteReference(holder, field, target);
    RegionInfo* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(holder));
    RegionInfo* targetPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(target));
    const bool implicit = page->IsAllocating();
    const bool live = productLive(page, holder);
    const bool targetLive = productLive(targetPage, target);
    const bool excluded = page->IsAllocating() && !page->IsKnownYoungEmpty();
    auto& productCollector = static_cast<WCollector&>(collector);
    MarkDomain* domain = productCollector.YoungMarkDomain();
    const size_t pendingBefore = domain->Stripes().Population() + domain->Stacks().Population();
    holder->OnFinalizerCreated();
    const size_t pendingAfter = domain->Stripes().Population() + domain->Stacks().Population();
    const bool noExplicitMark = !page->is_marked();
    const bool noPublication = pendingAfter == pendingBefore;
    std::fprintf(stderr, "P1_NEW_REGISTRATION_ASSERT_EXECUTED birth=%llu owner=%llu no_bitmap=%d "
                 "pending_before=%zu pending_after=%zu\n",
                 static_cast<unsigned long long>(page->BirthSequence()),
                 static_cast<unsigned long long>(page->GetSnapshotEpoch()), noExplicitMark,
                 pendingBefore, pendingAfter);
    const auto during = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG);
    const auto phase = during.phase;
    std::fprintf(stderr, "MARK_ALLOC_TARGET_ASSERT_EXECUTED existing=%d phase=%u young=%d large=%d "
                 "implicit=%d live=%d target_live=%d excluded=%d\n", existing, unsigned(phase),
                 page->IsYoungRegion(), page->IsLargeRegion(), implicit, live, targetLive, excluded);
    size_t markEndObservations = 0;
    bool markEndTargetLive = false;
    TracingCollector::testYoungMarkCompleted = [&, target, productLive]() {
        const auto markEnd = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG);
        if (markEnd.sequence != during.sequence) return;
        RegionInfo* endPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(target));
        markEndTargetLive = productLive(endPage, target);
        ++markEndObservations;
        std::fprintf(stderr, "MARK_ALLOC_MARK_END_ASSERT_EXECUTED sequence=%llu phase=%u live=%d\n",
                     static_cast<unsigned long long>(markEnd.sequence), unsigned(markEnd.phase), markEndTargetLive);
    };
    heap.RemoveExportObject(beforeSmallRoot);
    if (existing) heap.RemoveExportObject(targetRoot);
    mutator->SetManagedContext(false);
    MarkAllocationWindow::released.store(true, std::memory_order_release);
    bool completed;
    {
        ScopedEnterSaferegion safe(false);
        completed = MarkAllocationWindow::Wait(MarkAllocationWindow::completed);
    }
    // Wait through the real driver's acknowledgement, then check next-cycle
    // watermark resampling using the same rooted holder.
    collector.RequestGC(GC_REASON_YOUNG, false);
    SetMarkClosureObserverForTest(nullptr);
    TracingCollector::testYoungMarkCompleted = nullptr;
    holder = static_cast<MArray*>(heap.GetExportObject(holderRoot));
    page = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(holder));
    auto& completedField = HeapSlotAt<>(reinterpret_cast<uintptr_t>(holder->ConvertToCArray()));
    BaseObject* completedTarget = Heap::GetBarrier().ReadReference(holder, completedField);
    // The request includes relocation. Check the actual field result here;
    // the mark-end predicate was observed before relocation at the success exit.
    const bool completedValue = completedTarget != nullptr &&
        static_cast<MArray*>(completedTarget)->GetLength() == 16;
    std::fprintf(stderr, "MARK_ALLOC_COMPLETED_VALUE_ASSERT_EXECUTED length_valid=%d\n", completedValue);
    const bool resampled = !page->IsAllocating();
    const auto after = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG);
    const bool nextCycle = after.sequence > during.sequence;
    std::fprintf(stderr, "MARK_ALLOC_NEXT_CYCLE_ASSERT_EXECUTED before=%llu after=%llu resampled=%d\n",
                 static_cast<unsigned long long>(during.sequence),
                 static_cast<unsigned long long>(after.sequence), resampled);
    heap.RemoveExportObject(holderRoot);
    mutator->SetManagedContext(true);
    const uintptr_t status = (retiredTLAB ? 0 : 1024) | ((noExplicitMark && noPublication) ? 0 : 512) | (implicit ? 0 : 1) | (live ? 0 : 2) |
        (targetLive ? 0 : 4) | (excluded ? 0 : 8) | (nextCycle ? 0 : 16) |
        (resampled ? 0 : 32) |
        ((markEndObservations == 1 && markEndTargetLive) ? 0 : 128) |
        (completedValue ? 0 : 256) |
        ((completed && !MarkAllocationWindow::timedOut && phase == GC_PHASE_TRACE) ? 0 : 64);
    std::fprintf(stderr, "MARK_ALLOC_ASSERT_RESULT status=%zu "
                 "bits=implicit:1,live:2,target_live:4,excluded:8,next_cycle:16,resampled:32,window:64,mark_end_live:128,completed_value:256\n", status);
    return reinterpret_cast<void*>(status);
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
void ObservePinnedAllocationWindow()
{
    if (MarkAllocationWindow::entered.exchange(true)) return;
    MarkAllocationWindow::timedOut = !MarkAllocationWindow::Wait(MarkAllocationWindow::released);
    MarkAllocationWindow::completed.store(true, std::memory_order_release);
}

uint64_t pinnedAcquiredBirth = 0;
bool pinnedAcquiredWindow = false;

void PausePinnedPageBeforeInstall(RegionInfo* page)
{
    pinnedAcquiredBirth = page->BirthSequence();
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    Heap::GetHeap().GetCollector().RequestGC(GC_REASON_USER, true);
    {
        ScopedEnterSaferegion safe(false);
        pinnedAcquiredWindow = MarkAllocationWindow::Wait(MarkAllocationWindow::entered);
    }
    mutator->SetManagedContext(true);
}

void* RunPinnedPublicationCase(void*)
{
    auto& heap = Heap::GetHeap();
    auto& collector = heap.GetCollector();
    auto* mutator = Mutator::GetMutator();
    // Retire any existing shortcut through the real collector before acquiring
    // the new page. The hook only schedules a second real collection.
    mutator->SetManagedContext(false);
    collector.RequestGC(GC_REASON_USER, false);
    mutator->SetManagedContext(true);
    MarkAllocationWindow::entered = false;
    MarkAllocationWindow::released = false;
    MarkAllocationWindow::completed = false;
    MarkAllocationWindow::timedOut = false;
    pinnedAcquiredWindow = false;
    TracingCollector::testOldMarkStarted = ObservePinnedAllocationWindow;
    RegionManager::testPinnedPageAcquired = PausePinnedPageBeforeInstall;
    TypeInfo* type = GetReferenceArrayTypeInfos().component;
    const size_t size = AlignUp(type->GetInstanceSize() + TYPEINFO_PTR_SIZE, size_t{8});
    MObject* fresh = MObject::NewPinnedObject(type, size);
    RegionManager::testPinnedPageAcquired = nullptr;
    auto* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(fresh));
    MObject* next = MObject::NewPinnedObject(type, size);
    const bool reused = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(next)) == page;
    const bool current = page->IsAllocating();
    const bool advanced = page->GetSnapshotEpoch() > pinnedAcquiredBirth;
    const bool noMark = !page->is_marked();
    std::fprintf(stderr, "P1_PINNED_INSTALL_ASSERT_EXECUTED acquired=%llu birth=%llu owner=%llu "
        "advanced=%d current=%d reused=%d no_bitmap=%d window=%d\n",
        static_cast<unsigned long long>(pinnedAcquiredBirth),
        static_cast<unsigned long long>(page->BirthSequence()),
        static_cast<unsigned long long>(page->GetSnapshotEpoch()), advanced, current, reused, noMark,
        pinnedAcquiredWindow);
    const U64 root = heap.RegisterExportRoot(fresh);
    MarkAllocationWindow::released = true;
    mutator->SetManagedContext(false);
    {
        ScopedEnterSaferegion safe(false);
        MarkAllocationWindow::Wait(MarkAllocationWindow::completed);
    }
    collector.RequestGC(GC_REASON_USER, false);
    TracingCollector::testOldMarkStarted = nullptr;
    const bool retained = heap.GetExportObject(root) == fresh;
    heap.RemoveExportObject(root);
    mutator->SetManagedContext(true);
    return reinterpret_cast<void*>((advanced && current && reused && noMark && pinnedAcquiredWindow &&
        retained && !MarkAllocationWindow::timedOut) ? 0 : 1);
}

void* RunPinnedMarkStartCase(void*)
{
    auto& heap = Heap::GetHeap();
    auto& collector = heap.GetCollector();
    auto* mutator = Mutator::GetMutator();
    TypeInfo* type = GetReferenceArrayTypeInfos().component;
    const size_t size = AlignUp(type->GetInstanceSize() + TYPEINFO_PTR_SIZE, size_t{8});
    MObject* first = MObject::NewPinnedObject(type, size);
    MObject* second = MObject::NewPinnedObject(type, size);
    RegionInfo* before = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(first));
    const bool reused = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(second)) == before;
    const U64 root = heap.RegisterExportRoot(first);
    MarkAllocationWindow::entered = false;
    MarkAllocationWindow::released = false;
    MarkAllocationWindow::completed = false;
    MarkAllocationWindow::timedOut = false;
    TracingCollector::testOldMarkStarted = ObservePinnedAllocationWindow;
    mutator->SetManagedContext(false);
    collector.RequestGC(GC_REASON_USER, true);
    bool entered;
    {
        ScopedEnterSaferegion safe(false);
        entered = MarkAllocationWindow::Wait(MarkAllocationWindow::entered);
    }
    if (!entered) {
        MarkAllocationWindow::released = true;
        TracingCollector::testOldMarkStarted = nullptr;
        mutator->SetManagedContext(true);
        return reinterpret_cast<void*>(2);
    }
    mutator->SetManagedContext(true);
    MObject* fresh = MObject::NewPinnedObject(type, size);
    RegionInfo* after = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(fresh));
    const bool current = after->IsAllocating();
    const bool different = after != before;
    const bool noMark = !after->is_marked();
    const bool window = collector.GetCycleSnapshot(GCCycleGeneration::OLD).phase == GC_PHASE_TRACE;
    std::fprintf(stderr, "P1_PINNED_WINDOW_ASSERT_EXECUTED reuse=%d different=%d current=%d no_bitmap=%d trace=%d birth=%llu owner=%llu\n",
        reused, different, current, noMark, window,
        static_cast<unsigned long long>(after->BirthSequence()),
        static_cast<unsigned long long>(after->GetSnapshotEpoch()));
    MarkAllocationWindow::released = true;
    mutator->SetManagedContext(false);
    {
        ScopedEnterSaferegion safe(false);
        MarkAllocationWindow::Wait(MarkAllocationWindow::completed);
    }
    collector.RequestGC(GC_REASON_USER, false);
    TracingCollector::testOldMarkStarted = nullptr;
    const bool retained = heap.GetExportObject(root) == first;
    heap.RemoveExportObject(root);
    mutator->SetManagedContext(true);
    return reinterpret_cast<void*>((reused && different && current && noMark && window && retained &&
        !MarkAllocationWindow::timedOut) ? 0 : 1);
}
#endif

void* RunPinnedBirthCase(void*)
{
    auto& heap = Heap::GetHeap();
    auto* mutator = Mutator::GetMutator();
    TypeInfo* type = GetReferenceArrayTypeInfos().component;
    const size_t size = AlignUp(type->GetInstanceSize() + TYPEINFO_PTR_SIZE, size_t{8});
    MObject* dead = MObject::NewPinnedObject(type, size);
    MObject* survivor = MObject::NewPinnedObject(type, size);
    RegionInfo* oldPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(dead));
    const bool shared = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(survivor)) == oldPage;
    const U64 root = heap.RegisterExportRoot(survivor);
    mutator->SetManagedContext(false);
    heap.GetCollector().RequestGC(GC_REASON_USER, false);
    mutator->SetManagedContext(true);
    MObject* fresh = MObject::NewPinnedObject(type, size);
    RegionInfo* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(fresh));
    const bool fromNewPage = page != oldPage;
    const bool noExplicitMark = !page->is_marked();
    const bool allocating = page->IsAllocating();
    const bool retained = heap.GetExportObject(root) == survivor;
    std::fprintf(stderr, "P1_PINNED_ASSERT_EXECUTED shared=%d new_page=%d allocating=%d no_bitmap=%d retained=%d\n",
                 shared, fromNewPage, allocating, noExplicitMark, retained);
    heap.RemoveExportObject(root);
    return reinterpret_cast<void*>((shared && fromNewPage && allocating && noExplicitMark && retained) ? 0 : 1);
}

void* RunVisibleArrayGraph(void*)
{
    Mutator::GetMutator()->SetManagedContext(false);
    MArray* array = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    NativeSlot root(zpointer::null);
    Heap::GetBarrier().WriteStaticRef(root, array);
    NativeSlot* roots[] = { &root };
    Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    std::vector<size_t> visits(array->GetLength(), 0);
    size_t invalid = 0;
    size_t objects = 0;
    const MAddress first = reinterpret_cast<MAddress>(array->ConvertToCArray());
    {
        ScopedEnterSaferegion saferegion(false);
        ScopedStopTheWorld stw("segmented-array range graph", false);
        HeapIterator(false).Iterate([&](BaseObject* object) { objects += object == array; },
            [&](BaseObject* base, const void* slot, uintptr_t) {
                if (base != array) { return; }
                const MAddress field = reinterpret_cast<MAddress>(slot);
                if (field < first || (field - first) % sizeof(RefField<>) != 0 ||
                    (field - first) / sizeof(RefField<>) >= visits.size()) {
                    ++invalid;
                } else {
                    ++visits[(field - first) / sizeof(RefField<>)];
                }
            });
    }
    Heap::GetHeap().UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    for (size_t count : visits) { invalid += count != 1; }
    const bool complete = objects == 1 && invalid == 0;
    std::fprintf(stderr, "SEGMENTED_GRAPH_RANGE_ASSERT objects=%zu fields=%zu invalid=%zu pass=%d\n",
                 objects, visits.size(), invalid, complete);
    Mutator::GetMutator()->SetManagedContext(true);
    return reinterpret_cast<void*>(complete ? 0 : 1);
}

struct InvisibleGraphProbe {
    static size_t checks;
    static size_t objects;
    static void Publish(MArray* array)
    {
        // Construct legal null payloads so a deliberately wrong graph-root
        // inclusion reaches this test's target result without an earlier
        // field-value check. This fixture tests root selection, not clearing.
        std::memset(array->ConvertToCArray(), 0, array->GetContentSize());
    }
    static void Yield(size_t segment)
    {
        if (segment != 0 || checks != 0) { return; }
        ++checks;
        BaseObject* root = Mutator::GetMutator()->LoadInvisibleRoot();
        // This fixture runs on a registered runtime thread. The initializer's
        // onlyForMutator guard intentionally does not pause GC threads.
        ScopedEnterSaferegion saferegion(false);
        ScopedStopTheWorld stw("segmented-array invisible graph root", false);
        HeapIterator(false).Iterate([&](BaseObject* object) { objects += object == root; });
    }
};
size_t InvisibleGraphProbe::checks = 0;
size_t InvisibleGraphProbe::objects = 0;

void* RunInvisibleArrayGraph(void*)
{
    Mutator::GetMutator()->SetManagedContext(false);
    LargeArrayInitTestHooks hooks;
    hooks.onPublish = InvisibleGraphProbe::Publish;
    hooks.onYield = InvisibleGraphProbe::Yield;
    CJ_MRT_SetLargeArrayInitTestHooks(&hooks);
    (void)MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    CJ_MRT_SetLargeArrayInitTestHooks(nullptr);
    const bool excluded = InvisibleGraphProbe::checks == 1 && InvisibleGraphProbe::objects == 0;
    std::fprintf(stderr, "SEGMENTED_GRAPH_INVISIBLE_ASSERT checks=%zu objects=%zu pass=%d\n",
                 InvisibleGraphProbe::checks, InvisibleGraphProbe::objects, excluded);
    Mutator::GetMutator()->SetManagedContext(true);
    return reinterpret_cast<void*>(excluded ? 0 : 1);
}

int RunRuntimeCase(CJTaskFunc task, uintptr_t argument, U32 processorCount = 1,
                   bool runtimeThread = false)
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
        if (runtimeThread) {
            // Use the real native runtime-thread registration for graph tests.
            // RunCJTask stores a native FutureImpl in LWTData::obj; that is not
            // a managed heap-object root and is a separate scheduler/root issue.
            auto& manager = MutatorManager::Instance();
            manager.CreateRuntimeMutator(ThreadType::GC_THREAD);
            void* result = task(reinterpret_cast<void*>(argument));
            manager.DestroyRuntimeMutator(ThreadType::GC_THREAD);
            const uintptr_t status = reinterpret_cast<uintptr_t>(result);
            if (FiniCJRuntime() != E_OK) { _exit(103); }
            _exit(status > 99 ? 99 : static_cast<int>(status));
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
    (void)runtimeThread;
    return 0;
#endif
}
} // namespace

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(MarkAllocation, LargeHolderAndNewTargetAreImplicitlyLive)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunMarkAllocationCase, 0), 0);
}
GC_OTHER_VM_TEST(MarkAllocation, LargeHolderKeepsRootedExistingTargetLive)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunMarkAllocationCase, 1), 0);
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(LargePageGeneration, ArrayRootKeepsYoungTargetLive)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunLargeYoungClosureCase, 0), 0);
}
#endif

GC_OTHER_VM_TEST(P1Mark, PinnedReclaimedSlotIsNotAllocationSource)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunPinnedBirthCase, 0), 0);
}

GC_OTHER_VM_TEST(LargePageGeneration, ManagedAllocationPublishesYoungEden)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunLargePageIdentityCase, 0), 0);
}

GC_OTHER_VM_TEST(LargePageGeneration, NativeAllocationPublishesYoungEden)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunLargePageIdentityCase, 1), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, YieldKeepsInvisibleRootAndPublishesBoundary)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunSegmentedCase, static_cast<uintptr_t>(YieldGc::NONE)), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, VisibleArrayGraphUsesRangeChunks)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunVisibleArrayGraph, 0, 1, true), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, InvisibleRootIsExcludedFromHeapGraph)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunInvisibleArrayGraph, 0, 1, true), 0);
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

GC_OTHER_VM_TEST(SegmentedArrayInit, YoungGcWatermarkResidualFallbackIsUnreachable)
{
    constexpr uintptr_t requireWatermarkDone = 0x100U;
    GC_EXPECT_EQ(RunRuntimeCase(RunSegmentedCase,
                                requireWatermarkDone | static_cast<uintptr_t>(YieldGc::YOUNG)), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, SmallReferenceArrayKeepsFastPath)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunSmallReferenceCase, 0), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, NativeSmallReferenceArrayKeepsFastPath)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunSmallReferenceCase, 1), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, LargePrimitiveArrayUsesSegmentedClearing)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunLargePrimitiveCase, 0), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, NativeLargePrimitiveArrayUsesSegmentedClearing)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunLargePrimitiveCase, 1), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, TwoGcReferenceInitializationRestartsOnlyOnce)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunTwoGcReferenceCase, 0), 0);
}

GC_OTHER_VM_TEST(SegmentedArrayInit, TwoGcPrimitiveInitializationDoesNotRestart)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunLargePrimitiveCase, 2), 0);
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(P1Mark, PinnedPagePublicationAcrossMarkStart)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunPinnedPublicationCase, 0), 0);
}

GC_OTHER_VM_TEST(P1Mark, PinnedMarkStartRetiresAllocationPage)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunPinnedMarkStartCase, 0), 0);
}
#endif

#endif // MRT_GC_UNIT_TESTS
