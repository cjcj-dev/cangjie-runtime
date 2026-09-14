// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#if defined(MRT_GC_UNIT_TESTS)
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/Collector/GcRequest.h"
#include "Heap/z/zHeap.hpp"
#include "Mutator/Mutator.h"
#include "ObjectModel/MArray.inline.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
extern "C" ArrayRef MCC_NewObjArray(const TypeInfo* arrayInfo, MIndex nElems);
}

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
enum class ReceiptTarget { MARK, REMAP, FULL_SKIP, RANGE_SKIP };
constexpr MIndex kLength = MArray::LARGE_ARRAY_INIT_SEGMENT_SIZE * 2 / sizeof(RefField<>) + 1;

struct ReceiptTypes {
    alignas(TypeInfo) unsigned char componentBytes[sizeof(TypeInfo)] {};
    alignas(TypeInfo) unsigned char arrayBytes[sizeof(TypeInfo)] {};
    TypeInfo* component = reinterpret_cast<TypeInfo*>(componentBytes);
    TypeInfo* array = reinterpret_cast<TypeInfo*>(arrayBytes);

    ReceiptTypes()
    {
        component->SetType(TypeKind::TYPE_KIND_CLASS);
        component->SetInstanceSize(sizeof(void*));
        array->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        array->SetComponentTypeInfo(component);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(this), sizeof(*this));
    }
};

struct ReceiptCase {
    ReceiptTarget target;
    TypeInfo* arrayType = nullptr;
    bool yielded = false;
    bool requested = false;
    bool epochAdvanced = false;
    size_t setupFailures = 0;
    size_t incompleteFull = 0;
    size_t incompleteRange = 0;
    size_t completeFull = 0;
    size_t completeRange = 0;
    size_t nonNull = 0;
    LargeArrayInitRootReceipt receipt;
    MArray* completed = nullptr;
    bool targetPassed = false;
};
ReceiptCase* activeCase = nullptr;

void AtYield(size_t)
{
    ReceiptCase& test = *activeCase;
    if (test.yielded) {
        return;
    }
    test.yielded = true;
    MArray* array = static_cast<MArray*>(Mutator::GetMutator()->LoadInvisibleRoot());
    if (array == nullptr || !array->IsInvisibleObject() || array->GetLength() != kLength) {
        ++test.setupFailures;
        return;
    }
    // zIterator.inline.hpp:56-70. No element values are supplied by the test.
    array->ForEachRefField([&](RefField<>&) { ++test.incompleteFull; });
    const MAddress start = reinterpret_cast<MAddress>(array->ConvertToCArray());
    array->ForEachRefFieldInRange([&](RefField<>&) { ++test.incompleteRange; },
                                start, start + kLength * sizeof(RefField<>));
    if (test.target == ReceiptTarget::FULL_SKIP || test.target == ReceiptTarget::RANGE_SKIP) {
        return;
    }

    // A real young allocation prevents the empty-young fast path. All reference
    // publication uses runtime APIs; no Cangjie compiler or raw element store.
    U64 seedRoot = 0;
    {
        ScopedObjectAccess access;
        MArray* seed = MCC_NewObjArray(test.arrayType, 16);
        if (seed != nullptr) {
            seedRoot = Heap::GetHeap().RegisterExportRoot(seed);
        }
    }
    if (seedRoot == 0) {
        ++test.setupFailures;
        return;
    }
    Collector& collector = Heap::GetHeap().GetCollector();
    const uint64_t before = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG).sequence;
    test.requested = true;
    collector.RequestGC(GC_REASON_YOUNG, false);
    test.epochAdvanced = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG).sequence != before;
    Heap::GetHeap().RemoveExportObject(seedRoot);
}

void* AllocateAndObserve(void*)
{
    ReceiptCase& test = *activeCase;
    static ReceiptTypes types;
    test.arrayType = types.array;
    LargeArrayInitTestHooks hooks;
    hooks.onYield = AtYield;
    hooks.observeRootReceipt = true;
    CJ_MRT_SetLargeArrayInitTestHooks(&hooks);
    Mutator* mutator = Mutator::GetMutator();
    // C++ task frames have no Cangjie stack maps. Exercise the genuine native
    // side-root path, rather than fabricating managed metadata.
    mutator->SetManagedContext(false);
    MArray* array = MCC_NewObjArray(types.array, kLength);
    mutator->SetManagedContext(true);
    test.completed = array;
    CJ_MRT_ReadLargeArrayInitRootReceipt(&test.receipt);
    CJ_MRT_SetLargeArrayInitTestHooks(nullptr);
    if (array == nullptr || array->IsInvisibleObject() || array->GetLength() != kLength ||
        mutator->LoadInvisibleRoot() != nullptr) {
        ++test.setupFailures;
        return nullptr;
    }
    array->ForEachRefField([&](RefField<>& slot) {
        ++test.completeFull;
        test.nonNull += !is_null(slot.GetFieldValue()) ? 1 : 0;
    });
    const MAddress start = reinterpret_cast<MAddress>(array->ConvertToCArray());
    array->ForEachRefFieldInRange([&](RefField<>&) { ++test.completeRange; },
                                start, start + kLength * sizeof(RefField<>));
    const auto bit = [](LargeArrayRootVisitSite site) { return uint32_t { 1 } << static_cast<unsigned>(site); };
    switch (test.target) {
        case ReceiptTarget::MARK: {
            const uint32_t required = test.receipt.markWatermarkDone
                ? bit(LargeArrayRootVisitSite::STACK_WATERMARK_NATIVE)
                : bit(LargeArrayRootVisitSite::MUTATOR_STACK_NATIVE) | bit(LargeArrayRootVisitSite::MINOR_MARK);
            test.targetPassed = test.receipt.markPhases == 1 &&
                (test.receipt.markSites & required) == required;
            break;
        }
        case ReceiptTarget::REMAP:
            test.targetPassed = (test.receipt.remapSites & bit(LargeArrayRootVisitSite::STACK_WATERMARK_NATIVE)) != 0 &&
                test.receipt.remapRoot == array;
            break;
        case ReceiptTarget::FULL_SKIP:
            test.targetPassed = test.incompleteFull == 0;
            break;
        case ReceiptTarget::RANGE_SKIP:
            test.targetPassed = test.incompleteRange == 0;
            break;
    }
    return nullptr;
}

void CheckReceipt(ReceiptTarget target, U32 processors = 1)
{
    ReceiptCase test { target };
    activeCase = &test;
    (void)setenv("cjProcessorNum", processors == 1 ? "1" : "2", 1);
    RuntimeParam param {};
    param.heapParam.heapSize = 32 * 1024;
    param.coParam.processorNum = processors;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    CJThreadHandle handle = RunCJTask(AllocateAndObserve, nullptr);
    GC_EXPECT_TRUE(handle != nullptr);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &result), E_OK);
    ReleaseHandle(handle);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
    activeCase = nullptr;
    const bool needsGc = target == ReceiptTarget::MARK || target == ReceiptTarget::REMAP;
    const bool setup = test.setupFailures == 0 && test.yielded &&
        (!needsGc || (test.requested && test.epochAdvanced)) &&
        test.completeFull == kLength && test.completeRange == kLength && test.nonNull == 0;
    std::fprintf(stderr,
        "[SEGMENTED_RECEIPT_ASSERT] target=%u passed=%d setup=%d mark=%#x remap=%#x "
        "mark_phase_n=%zu watermark_done=%d remap_root=%p completed=%p "
        "incomplete_full=%zu incomplete_range=%zu complete_full=%zu complete_range=%zu nonnull=%zu\n",
        static_cast<unsigned>(target), test.targetPassed, setup, test.receipt.markSites, test.receipt.remapSites,
        test.receipt.markPhases, test.receipt.markWatermarkDone, static_cast<void*>(test.receipt.remapRoot),
        static_cast<void*>(test.completed), test.incompleteFull, test.incompleteRange,
        test.completeFull, test.completeRange, test.nonNull);
    // Every phase cut must arrive here with setup=1 and fail this target check.
    GC_EXPECT_TRUE(test.targetPassed);
    GC_EXPECT_TRUE(setup);
}
} // namespace

GC_OTHER_VM_TEST(SegmentedArrayRootReceipt, YoungMarkConsumption) { CheckReceipt(ReceiptTarget::MARK); }
GC_OTHER_VM_TEST(SegmentedArrayRootReceipt, YoungRemapConsumption) { CheckReceipt(ReceiptTarget::REMAP); }
GC_OTHER_VM_TEST(SegmentedArrayRootReceipt, YoungMarkConsumptionParallel) { CheckReceipt(ReceiptTarget::MARK, 2); }
GC_OTHER_VM_TEST(SegmentedArrayRootReceipt, YoungRemapConsumptionParallel) { CheckReceipt(ReceiptTarget::REMAP, 2); }
GC_OTHER_VM_TEST(SegmentedArrayRootReceipt, IncompleteFullIteratorSkip) { CheckReceipt(ReceiptTarget::FULL_SKIP); }
GC_OTHER_VM_TEST(SegmentedArrayRootReceipt, IncompleteRangeIteratorSkip) { CheckReceipt(ReceiptTarget::RANGE_SKIP); }
#endif
