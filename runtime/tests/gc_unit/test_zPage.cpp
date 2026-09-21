// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_heap_fixture.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zPageTable.hpp"
#include "Heap/z/zPageType.hpp"
#include "gc_unittest.hpp"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/MObject.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(ZPageType, EnumValues)
{
    GC_EXPECT_EQ(static_cast<uint8_t>(ZPageType::small), 0);
    GC_EXPECT_EQ(static_cast<uint8_t>(ZPageType::medium), 1);
    GC_EXPECT_EQ(static_cast<uint8_t>(ZPageType::large), 2);
}

GC_TEST(ZPage, AllocPagePublishedInTable)
{
    GcHeapFixture fx;
    ZPage* page = fx.region0;
    GC_EXPECT_TRUE(page != nullptr);
    GC_EXPECT_TRUE(ZPageTable::heap_table().get(page->GetRegionStart()) == page);
    GC_EXPECT_TRUE(Heap::page(page->GetRegionStart()) == page);
    page->reset_seqnum();
    GC_EXPECT_TRUE(page->is_allocating());
    GC_EXPECT_TRUE(!page->is_relocatable());
}

GC_TEST(ZPage, ObjectAlignmentFollowsType)
{
    GcHeapFixture fx;
    ZPage* page = fx.region0;
    GC_EXPECT_EQ(page->object_alignment(), size_t(1) << page->object_alignment_shift());
    if (page->is_small()) {
        GC_EXPECT_EQ(page->object_alignment_shift(), ZObjectAlignmentSmallShift);
    }
}

GC_TEST(ZPage, AllocObjectRespectsAlignment)
{
    GcHeapFixture fx;
    ZPage* page = fx.region0;
    page->reset_seqnum();
    const uintptr_t addr = page->alloc_object(16);
    GC_EXPECT_TRUE(addr != 0);
    GC_EXPECT_EQ(addr % page->object_alignment(), 0u);
    GC_EXPECT_TRUE(page->undo_alloc_object(addr, 16));
}

// Real mutator entry -> object/page allocators -> page table. Values are copied
// out by the mutator and asserted after joining, without constructing a page or
// feeding an intermediate geometry into the consumer.
#include "Cangjie.h"
#include "TypeInfoManager.h"
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include "Heap/z/zJNICritical.hpp"
#include "Mutator/MutatorManager.h"
#include "Mutator/Mutator.inline.h"

namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo* klass, MSize size);
extern "C" ArrayRef MCC_NewArray8(const TypeInfo* arrayInfo, MIndex nElems);
extern "C" void* MCC_AcquireRawData(ArrayRef array, bool* isCopy);
extern "C" void MCC_ReleaseRawData(ArrayRef array, void* rawPtr);
}
namespace {
struct GranuleAllocationResult {
    size_t requested[3]{};
    size_t expected[3]{};
    size_t actual[3]{};
    unsigned type[3]{};
    bool allocated[3]{};
    bool firstMapped[3]{};
    bool lastMapped[3]{};
    bool sameSmallPage{false};
    size_t tableSize{0};
};

struct JNICriticalBlockedEnterResult {
    std::atomic<bool> ready{false};
    std::atomic<bool> proceed{false};
    std::atomic<bool> entering{false};
    std::atomic<bool> acquired{false};
    bool copied{true};
};

void* AcquireWhileJNICriticalBlocked(void* context)
{
    auto& result = *static_cast<JNICriticalBlockedEnterResult*>(context);
    alignas(TypeInfo) static unsigned char byteStorage[sizeof(TypeInfo)]{};
    alignas(TypeInfo) static unsigned char arrayStorage[sizeof(TypeInfo)]{};
    auto* byteType = reinterpret_cast<TypeInfo*>(byteStorage);
    auto* arrayType = reinterpret_cast<TypeInfo*>(arrayStorage);
    byteType->SetType(TypeKind::TYPE_KIND_UINT8);
    byteType->SetInstanceSize(1);
    arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    arrayType->SetComponentTypeInfo(byteType);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(byteStorage), sizeof(byteStorage));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(arrayStorage), sizeof(arrayStorage));
    auto* array = static_cast<MArray*>(MCC_NewArray8(arrayType, 64));
    result.ready.store(true, std::memory_order_release);
    while (!result.proceed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    result.entering.store(true, std::memory_order_release);
    void* raw = MCC_AcquireRawData(array, &result.copied);
    result.acquired.store(true, std::memory_order_release);
    MCC_ReleaseRawData(array, raw);
    return nullptr;
}
void* AllocateGranulePages(void* context)
{
    auto& result = *static_cast<GranuleAllocationResult*>(context);
    alignas(TypeInfo) static unsigned char types[3][sizeof(TypeInfo)];
    result.requested[0] = 32;
    result.requested[1] = ZObjectSizeLimitSmall + 16;
    result.requested[2] = ZObjectSizeLimitMedium + ZGranuleSize + 16;
    result.expected[0] = ZPageSizeSmall;
    // zObjectAllocator.cpp:144: a cold medium request uses the maximum page size.
    // Cache-only variable sizes are covered by FastMediumConsumesCachedActualSize.
    result.expected[1] = ZPageSizeMediumMax;
    result.expected[2] = AlignUp(result.requested[2], ZGranuleSize);
    ZPage* first = nullptr;
    for (size_t i = 0; i < 3; ++i) {
        std::memset(types[i], 0, sizeof(types[i]));
        auto* type = reinterpret_cast<TypeInfo*>(types[i]);
        type->SetType(TypeKind::TYPE_KIND_CLASS);
        type->SetInstanceSize(result.requested[i] - TYPEINFO_PTR_SIZE);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(types[i]), sizeof(types[i]));
        const ObjRef object = MCC_NewObject(type, result.requested[i]);
        result.allocated[i] = object != nullptr;
        ZPage* page = object == nullptr ? nullptr : Heap::page(reinterpret_cast<uintptr_t>(object));
        result.firstMapped[i] = page != nullptr;
        if (page != nullptr) {
            result.actual[i] = page->size();
            result.type[i] = static_cast<unsigned>(page->type());
            result.lastMapped[i] = Heap::page(page->GetRegionEnd() - 1) == page;
        }
        if (i == 0) { first = page; }
    }
    const ObjRef smallAgain = MCC_NewObject(reinterpret_cast<TypeInfo*>(types[0]), result.requested[0]);
    result.sameSmallPage = first != nullptr && Heap::page(reinterpret_cast<uintptr_t>(smallAgain)) == first;
    result.tableSize = Heap::page_table().map().size();
    return nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(ZPageGranule, MutatorAllocatesThreePageSizes)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    GranuleAllocationResult result;
    CJThreadHandle handle = RunCJTask(AllocateGranulePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "GRANULE_TARGET table=%zu expected=%zu shared_small=%d\n",
                 result.tableSize, ZAddressOffsetMax >> ZGranuleSizeShift, result.sameSmallPage);
    // No fatal page-existence assertion precedes the geometry assertions.
    for (size_t i = 0; i < 3; ++i) {
        std::fprintf(stderr, "GRANULE_TARGET class=%zu allocated=%d mapped=%d last=%d bytes=%zu expected=%zu type=%u\n",
                     i, result.allocated[i], result.firstMapped[i], result.lastMapped[i],
                     result.actual[i], result.expected[i], result.type[i]);
        GC_EXPECT_EQ(result.actual[i], result.expected[i]);
        GC_EXPECT_EQ(result.type[i], static_cast<unsigned>(i));
        GC_EXPECT_TRUE(result.firstMapped[i] && result.lastMapped[i]);
    }
    GC_EXPECT_EQ(result.tableSize, ZAddressOffsetMax >> ZGranuleSizeShift);
    GC_EXPECT_TRUE(result.sameSmallPage);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
struct ForwardingSelectionResult {
    size_t roots{0};
    size_t published{0};
    size_t retained{0};
    size_t retired{0};
    size_t receipts{0};
    size_t remapReceipts{0};
    bool verifyRetirement{false};
    bool verifyPin{false};
    bool verifyCritical{false};
    bool collectionFinishedWhileHeld{false};
    bool movedWhileHeld{false};
    bool movedAfterRelease{false};
    bool collectionFinished{false};
    uintptr_t pinExpected{0};
    uintptr_t pinObserved{0};
    bool pinCopied{true};
    size_t usedBefore{0};
    size_t usedAfter{0};
    size_t mappedBefore{0};
    size_t mappedAfter{0};
    size_t generationBefore[2]{};
    size_t generationAfter[2]{};
    size_t mappedGenerationBefore[2]{};
    size_t mappedGenerationAfter[2]{};
    size_t reused{0};
    size_t completed{0};
    size_t pending{0};
};
void* SelectRealLivePages(void* context)
{
    auto& result = *static_cast<ForwardingSelectionResult*>(context);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)];
    std::memset(storage, 0, sizeof(storage));
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(4096 - TYPEINFO_PTR_SIZE);
    alignas(TypeInfo) static unsigned char sourceByteStorage[sizeof(TypeInfo)]{};
    if (result.verifyPin || result.verifyCritical) {
        auto* byteType = reinterpret_cast<TypeInfo*>(sourceByteStorage);
        byteType->SetType(TypeKind::TYPE_KIND_UINT8);
        byteType->SetInstanceSize(1);
        type->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        type->SetComponentTypeInfo(byteType);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(sourceByteStorage), sizeof(sourceByteStorage));
    }
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    uintptr_t starts[3]{};
    U64 roots[3]{};
    ZPage* previous = nullptr;
    for (size_t i = 0; i < 4 * ZPageSizeSmall / 4096 && result.roots < 3; ++i) {
        BaseObject* object = (result.verifyPin || result.verifyCritical) ? static_cast<BaseObject*>(MCC_NewArray8(type, 4096))
                                             : static_cast<BaseObject*>(MCC_NewObject(type, 4096));
        if (object == nullptr) { return nullptr; }
        ZPage* page = Heap::page(reinterpret_cast<uintptr_t>(object));
        if (page != previous) {
            starts[result.roots] = reinterpret_cast<uintptr_t>(object);
            if (result.verifyPin || result.verifyCritical) {
                static_cast<MArray*>(object)->ConvertToCArray()[0] = result.roots + 1;
            } else {
                *reinterpret_cast<uint64_t*>(starts[result.roots] + TYPEINFO_PTR_SIZE) = result.roots + 1;
            }
            roots[result.roots] = Heap::GetHeap().RegisterExportRoot(reinterpret_cast<BaseObject*>(object));
            ++result.roots;
            previous = page;
        }
    }
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    auto snapshot = [](size_t& used, size_t& mapped, size_t* generations, size_t* mappedGenerations) {
        auto& allocator = Heap::GetHeap().page_allocator();
        used = allocator.GetUsedBytes();
        generations[0] = allocator.used_generation(ZGenerationId::young);
        generations[1] = allocator.used_generation(ZGenerationId::old);
        ZPageTableIterator iter(&Heap::page_table());
        for (ZPage* page; iter.next(&page);) {
            mapped += page->size();
            mappedGenerations[page->generation_id() == ZGenerationId::young ? 0 : 1] += page->size();
        }
    };
    if (result.verifyRetirement) {
        snapshot(result.usedBefore, result.mappedBefore, result.generationBefore, result.mappedGenerationBefore);
    }
    // Live objects in three real small pages force a non-empty relocation set.
    if (result.verifyCritical) {
        auto* array = static_cast<MArray*>(Heap::GetHeap().GetExportObject(roots[0]));
        bool copied = true;
        void* raw = MCC_AcquireRawData(array, &copied);
        std::atomic<bool> finished{false};
        // Let STW proceed: the JNI gate must exclude relocation, rather than
        // an uncooperative mutator preventing the pause from starting.
        mutator->EnterSaferegion(false);
        std::thread collector([&] {
            Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
            finished.store(true, std::memory_order_release);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!finished.load(std::memory_order_acquire) &&
               ZJNICritical::count_snapshot() != -2 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        result.collectionFinishedWhileHeld = finished.load(std::memory_order_acquire);
        // A completed collector publishes the root before we inspect it.
        if (result.collectionFinishedWhileHeld) {
            result.movedWhileHeld = reinterpret_cast<uintptr_t>(
                Heap::GetHeap().GetExportObject(roots[0])) != starts[0];
        }
        mutator->LeaveSaferegion();
        array = static_cast<MArray*>(Heap::GetHeap().GetExportObject(roots[0]));
        MCC_ReleaseRawData(array, raw);
        mutator->EnterSaferegion(false);
        collector.join();
        mutator->LeaveSaferegion();
        result.collectionFinished = finished.load(std::memory_order_acquire);
        result.movedAfterRelease = reinterpret_cast<uintptr_t>(
            Heap::GetHeap().GetExportObject(roots[0])) != starts[0];
    } else {
        Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
    }
    for (size_t i = 0; i < result.roots; ++i) {
        ZForwarding* forwarding = Heap::GetHeap().young().forwarding_table().get(starts[i]);
        if (forwarding != nullptr) {
            ++result.published;
            result.completed += forwarding->is_done() && forwarding->ref_count().load() == 0 &&
                forwarding->find(starts[i]) != 0;
            result.retired += Heap::page(starts[i]) == nullptr;
            if (result.verifyRetirement) {
                // Observe the remap result before a second barrier consumes it.
                // Otherwise a disconnected lookup fails in that barrier before
                // the test can assert which product result was wrong.
                BaseObject* resolved = ZGeneration::young()->relocate_or_remap_object(
                    reinterpret_cast<BaseObject*>(starts[i]));
                const MAddress target = reinterpret_cast<MAddress>(resolved);
                result.receipts += target != starts[i] && Heap::page(target) != nullptr &&
                    (result.verifyPin ? static_cast<MArray*>(resolved)->ConvertToCArray()[0] == i + 1 :
                     *reinterpret_cast<uint64_t*>(target + TYPEINFO_PTR_SIZE) == i + 1);
                BaseObject* const remapped = ZGeneration::young()->remap_object(
                    reinterpret_cast<BaseObject*>(starts[i]));
                const MAddress remapTarget = reinterpret_cast<MAddress>(remapped);
                result.remapReceipts += remapTarget != starts[i] && Heap::page(remapTarget) != nullptr &&
                    (result.verifyPin ? static_cast<MArray*>(remapped)->ConvertToCArray()[0] == i + 1 :
                     *reinterpret_cast<uint64_t*>(remapTarget + TYPEINFO_PTR_SIZE) == i + 1);
            }

        }
        if (!result.verifyRetirement) {
            result.retained += Heap::GetHeap().GetExportObject(roots[i]) != nullptr;
            Heap::GetHeap().RemoveExportObject(roots[i]);
        }
        // The retirement test leaves root cleanup to FiniCJRuntime, after its
        // assertions. A failing remap must not be consumed by cleanup first.
    }
    result.pending = generation_relocate_queue(Generation::Old).PendingCount() +
                     generation_relocate_queue(Generation::Young).PendingCount();
    if (result.verifyRetirement) {
        snapshot(result.usedAfter, result.mappedAfter, result.generationAfter, result.mappedGenerationAfter);
        // Real mutator allocation must be able to consume the returned source
        // range. Read forwarding results above before reusing that range.
        alignas(TypeInfo) static unsigned char byteStorage[sizeof(TypeInfo)]{};
        alignas(TypeInfo) static unsigned char arrayStorage[sizeof(TypeInfo)]{};
        auto* byteType = reinterpret_cast<TypeInfo*>(byteStorage);
        auto* arrayType = reinterpret_cast<TypeInfo*>(arrayStorage);
        byteType->SetType(TypeKind::TYPE_KIND_UINT8);
        byteType->SetInstanceSize(1);
        arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        arrayType->SetComponentTypeInfo(byteType);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(byteStorage), sizeof(byteStorage));
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(arrayStorage), sizeof(arrayStorage));
        for (size_t i = 0; i < 4 * ZPageSizeSmall / 4096 && result.reused == 0; ++i) {
            BaseObject* object = (result.verifyPin || result.verifyCritical) ? static_cast<BaseObject*>(MCC_NewArray8(arrayType, 4096))
                                                 : static_cast<BaseObject*>(MCC_NewObject(type, 4096));
            const uintptr_t address = reinterpret_cast<uintptr_t>(object);
            for (size_t j = 0; j < result.roots; ++j) {
                result.reused += (address >> ZGranuleSizeShift) == (starts[j] >> ZGranuleSizeShift);
            }
            if (result.verifyPin && result.reused != 0) {
                auto* array = static_cast<MArray*>(object);
                result.pinExpected = reinterpret_cast<uintptr_t>(array->ConvertToCArray());
                void* raw = MCC_AcquireRawData(array, &result.pinCopied);
                result.pinObserved = reinterpret_cast<uintptr_t>(raw);
                MCC_ReleaseRawData(array, raw);
            }
        }
    }
    mutator->SetManagedContext(true);
    return nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(ZForwardingPublication, SelectionPublishesPreparedForwardingOnce)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ForwardingSelectionResult result;
    CJThreadHandle handle = RunCJTask(SelectRealLivePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "FORWARDING_SELECTION_TARGET roots=%zu published=%zu retained=%zu\n",
                 result.roots, result.published, result.retained);
    GC_EXPECT_TRUE(result.published > 0);
    GC_EXPECT_EQ(result.retained, 3u);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// The product mutator allocates and roots the objects; RequestGC owns selection,
// copying, detach and retirement. No manually installed forwarding is involved.
GC_RUNTIME_OTHER_VM_TEST(ZRelocationRetirement, CopiedSourceLeavesPageTable)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ForwardingSelectionResult result;
    result.verifyRetirement = true;
    CJThreadHandle handle = RunCJTask(SelectRealLivePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "SOURCE_RETIREMENT_TARGET roots=%zu selected=%zu retired=%zu remapped_payloads=%zu\n",
                 result.roots, result.published, result.retired, result.receipts);
    GC_EXPECT_TRUE(result.published > 0);
    GC_EXPECT_EQ(result.retired, result.published);
    std::fprintf(stderr, "FORWARD_RESULT_TARGET relocated=%zu remapped=%zu expected=%zu\n",
                 result.receipts, result.remapReceipts, result.published);
    GC_EXPECT_EQ(result.receipts, result.published);
    GC_EXPECT_EQ(result.remapReceipts, result.published);
    std::fprintf(stderr, "SOURCE_MEMORY_TARGET used_before=%zu used_after=%zu mapped_before=%zu mapped_after=%zu reused=%zu\n",
                 result.usedBefore, result.usedAfter, result.mappedBefore, result.mappedAfter, result.reused);
    GC_EXPECT_EQ(result.usedAfter + result.mappedBefore, result.usedBefore + result.mappedAfter);
    for (size_t i = 0; i < 2; ++i) {
        std::fprintf(stderr, "SOURCE_GENERATION_TARGET id=%zu before=%zu after=%zu mapped_before=%zu mapped_after=%zu\n",
                     i, result.generationBefore[i], result.generationAfter[i],
                     result.mappedGenerationBefore[i], result.mappedGenerationAfter[i]);
        GC_EXPECT_EQ(result.generationAfter[i] + result.mappedGenerationBefore[i],
                     result.generationBefore[i] + result.mappedGenerationAfter[i]);
    }
    GC_EXPECT_TRUE(result.reused > 0);
    GC_EXPECT_EQ(result.roots, 3u);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
struct OldCyclePhaseResult {
    uintptr_t before{0};
    uintptr_t after{0};
    bool relocate{false};
};
void* RunRealOldCycle(void* context)
{
    auto& result = *static_cast<OldCyclePhaseResult*>(context);
    result.before = ZPointerRemappedOldMask;
    Mutator::GetMutator()->SetManagedContext(false);
    Heap::GetHeap().RequestGC(GC_REASON_USER, false);
    result.after = ZPointerRemappedOldMask;
    result.relocate = Heap::GetHeap().old().is_phase_relocate();
    Mutator::GetMutator()->SetManagedContext(true);
    return nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(ZGenerationPhases, OldCycleFlipsRemapMaskOnce)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    OldCyclePhaseResult result;
    CJThreadHandle handle = RunCJTask(RunRealOldCycle, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "OLD_CYCLE_FLIP_TARGET before=%#lx after=%#lx expected=%#lx relocate=%d\n",
                 result.before, result.after, result.before ^ ZPointerRemappedMask, result.relocate);
    GC_EXPECT_EQ(result.after, result.before ^ ZPointerRemappedMask);
    GC_EXPECT_TRUE(result.relocate);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// ZGC jni.cpp:2868-2886 resolves before pinning. The real allocator reuses a
// retired source range while its forwarding remains published for old colours.
GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, NewArrayOnReusedSourceKeepsDecodedAddress)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ForwardingSelectionResult result;
    result.verifyRetirement = true;
    result.verifyPin = true;
    CJThreadHandle handle = RunCJTask(SelectRealLivePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "PIN_DECODED_TARGET expected=%#lx observed=%#lx reused=%zu copied=%d\n",
                 result.pinExpected, result.pinObserved, result.reused, result.pinCopied);
    GC_EXPECT_EQ(result.pinObserved, result.pinExpected);
    GC_EXPECT_TRUE(result.pinExpected != 0 && result.reused > 0);
    GC_EXPECT_FALSE(result.pinCopied);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// ZGC zJNICritical.cpp:102-129: a JavaThread that finds JNI critical blocked
// transitions to blocked before taking the condition lock. Use RunCJTask so
// the waiter enters through the product MCC_AcquireRawData path.
GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, BlockedNewRawAcquireAllowsStopTheWorld)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    JNICriticalBlockedEnterResult result;
    CJThreadHandle handle = RunCJTask(AcquireWhileJNICriticalBlocked, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!result.ready.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < readyDeadline) {
        std::this_thread::yield();
    }
    GC_EXPECT_TRUE(result.ready.load(std::memory_order_acquire));
    ZJNICritical::block();
    result.proceed.store(true, std::memory_order_release);
    while (!result.entering.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::atomic<bool> stwFinished{false};
    std::thread collector([&] {
        ScopedStopTheWorld stw("jni-critical-blocked-enter", false);
        stwFinished.store(true, std::memory_order_release);
    });
    const auto stwDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!stwFinished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < stwDeadline) {
        std::this_thread::yield();
    }
    const bool stwFinishedWhileBlocked = stwFinished.load(std::memory_order_acquire);
    const bool acquiredWhileBlocked = result.acquired.load(std::memory_order_acquire);
    std::fprintf(stderr, "JNI_BLOCKED_ENTER_TARGET stw=%d acquired=%d count=%lld\n",
                 stwFinishedWhileBlocked ? 1 : 0, acquiredWhileBlocked ? 1 : 0,
                 static_cast<long long>(ZJNICritical::count_snapshot()));
    ZJNICritical::unblock();
    collector.join();
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    GC_EXPECT_TRUE(stwFinishedWhileBlocked);
    GC_EXPECT_FALSE(acquiredWhileBlocked);
    GC_EXPECT_TRUE(result.acquired.load(std::memory_order_acquire));
    GC_EXPECT_FALSE(result.copied);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

#if defined(MRT_TESTABLE_INTERNALS)
// ZGC zRelocate.cpp:1036-1047: a real worker owns detach/free/done.
// Pages come from the product allocator and the driver runs the actual tasks.
GC_RUNTIME_OTHER_VM_TEST(RelocateWorkers, RuntimeCollectionCompletesSelectedPages)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ForwardingSelectionResult result;
    result.verifyRetirement = true;
    CJThreadHandle handle = RunCJTask(SelectRealLivePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "ACTUAL_FORWARD_TASK_TARGET selected=%zu completed=%zu pending=%zu receipts=%zu\n",
                 result.published, result.completed, result.pending, result.receipts);
    GC_EXPECT_EQ(result.completed, result.published);
    GC_EXPECT_EQ(result.receipts, result.published);
    GC_EXPECT_EQ(result.pending, 0u);
    GC_EXPECT_TRUE(result.published > 0);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
#endif

// ZGC zCollectedHeap.cpp:275-280 and zGeneration.cpp:474-486.
GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, RawHolderExcludesCollectionRelocation)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ForwardingSelectionResult result;
    result.verifyCritical = true;
    CJThreadHandle handle = RunCJTask(SelectRealLivePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "JNI_RELOCATION_TARGET held_finished=%d held_moved=%d released_finished=%d released_moved=%d roots=%zu\n",
                 result.collectionFinishedWhileHeld, result.movedWhileHeld,
                 result.collectionFinished, result.movedAfterRelease, result.roots);
    GC_EXPECT_FALSE(result.movedWhileHeld);
    GC_EXPECT_FALSE(result.collectionFinishedWhileHeld);
    GC_EXPECT_TRUE(result.collectionFinished && result.movedAfterRelease);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// Exercise reflection through its compiler entry and the actual N2C stub.
#include "ObjectModel/MethodInfo.h"
#include "ObjectModel/FieldInfo.h"
namespace MapleRuntime {
extern "C" void* MCC_GetParameterAnnotations(ParameterInfo*, TypeInfo*);
extern "C" void* MCC_GetMethodAnnotations(MethodInfo*, TypeInfo*);
extern "C" void* MCC_GetInstanceFieldAnnotations(InstanceFieldInfo*, TypeInfo*);
extern "C" void* MCC_GetStaticFieldAnnotations(StaticFieldInfo*, TypeInfo*);
extern "C" void* MCC_GetTypeInfoAnnotations(TypeInfo*, TypeInfo*);
}
namespace {
struct AnnotationResult {
    unsigned entry = 0;
    TypeInfo* type = nullptr;
    uintptr_t before = 0;
    uintptr_t after = 0;
    uintptr_t returned = 0;
    bool called = false;
    bool copied = false;
};
thread_local AnnotationResult* annotationResult;
extern "C" void AnnotationCollect(uintptr_t* result)
{
    auto& r = *annotationResult;
    r.called = true;
    auto* mutator = Mutator::GetMutator();
    // Capture the registered slot once, independently of GC's process_head
    // visitor. The destructive arm changes only GC refresh, never this read.
    RootSlot* registeredSlot = nullptr;
    mutator->VisitMutatorRoots([&](RootSlot& root) {
        BaseObject* object = to_object(safe(root.LoadPlain()));
        if (object != nullptr && object->GetTypeInfo() == r.type) {
            registeredSlot = &root;
        }
    });
    r.before = registeredSlot == nullptr ? 0 : raw(registeredSlot->LoadPlain());
    std::fprintf(stderr, "ANNOTATION_HANDLE_PRECONDITION registered=%d before=%zx\n",
        registeredSlot != nullptr, r.before);
    {
        alignas(TypeInfo) static unsigned char garbageStorage[sizeof(TypeInfo)]{};
        auto* garbage = reinterpret_cast<TypeInfo*>(garbageStorage);
        garbage->SetType(TypeKind::TYPE_KIND_CLASS);
        garbage->SetInstanceSize(4096 - TYPEINFO_PTR_SIZE);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(garbageStorage), sizeof(garbageStorage));
        U64 roots[3]{};
        size_t count = 0;
        ZPage* previous = Heap::page(r.before);
        for (size_t i = 0; i < 4 * ZPageSizeSmall / 4096 && count < 3; ++i) {
            auto* object = MCC_NewObject(garbage, 4096);
            auto* page = Heap::page(reinterpret_cast<uintptr_t>(object));
            if (page != previous) {
                roots[count++] = Heap::GetHeap().RegisterExportRoot(object);
                previous = page;
            }
        }
        Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
        r.after = registeredSlot == nullptr ? 0 : raw(registeredSlot->LoadPlain());
        auto* forwarding = Heap::GetHeap().young().forwarding_table().get(r.before);
        std::fprintf(stderr, "ANNOTATION_FORWARDING from=%zx winner=%zx root=%zx\n", r.before,
            forwarding == nullptr ? 0 : forwarding->find(r.before), r.after);
        for (size_t i = 0; i < count; ++i) { Heap::GetHeap().RemoveExportObject(roots[i]); }
    }
    result[0] = 0;
    result[1] = 581;
    result[2] = 584;
}
void* RunAnnotation(void* context)
{
    auto& r = *static_cast<AnnotationResult*>(context);
    annotationResult = &r;
    Mutator::GetMutator()->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_STRUCT);
    type->SetInstanceSize(3 * sizeof(uintptr_t));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    r.type = type;
    // Metadata uses the compiler's packed ABI, with the annotation code pointer
    // in the declared field. No product access hook or alternate stub is used.
    uintptr_t callback = reinterpret_cast<uintptr_t>(&AnnotationCollect);
    void* object = nullptr;
    if (r.entry == 0) {
        ParameterInfo metadata{};
        std::memcpy(reinterpret_cast<char*>(&metadata) + sizeof(metadata) - sizeof(callback), &callback, sizeof(callback));
        object = MCC_GetParameterAnnotations(&metadata, type);
    } else if (r.entry == 1) {
        MethodInfo metadata{};
        std::memcpy(reinterpret_cast<char*>(&metadata) + 32, &callback, sizeof(callback));
        object = MCC_GetMethodAnnotations(&metadata, type);
    } else if (r.entry == 2) {
        InstanceFieldInfo metadata{};
        std::memcpy(reinterpret_cast<char*>(&metadata) + sizeof(metadata) - sizeof(callback), &callback, sizeof(callback));
        object = MCC_GetInstanceFieldAnnotations(&metadata, type);
    } else if (r.entry == 3) {
        StaticFieldInfo metadata{};
        std::memcpy(reinterpret_cast<char*>(&metadata) + sizeof(metadata) - sizeof(callback), &callback, sizeof(callback));
        object = MCC_GetStaticFieldAnnotations(&metadata, type);
    } else {
        ReflectInfo metadata{};
        std::memcpy(reinterpret_cast<char*>(&metadata) + 24, &callback, sizeof(callback));
        alignas(TypeInfo) unsigned char annotatedStorage[sizeof(TypeInfo)]{};
        auto* annotated = reinterpret_cast<TypeInfo*>(annotatedStorage);
        annotated->SetType(TypeKind::TYPE_KIND_CLASS);
        annotated->SetFlag(FLAG_REFLECTION);
        annotated->SetReflectInfo(&metadata);
        object = MCC_GetTypeInfoAnnotations(annotated, type);
    }
    r.returned = reinterpret_cast<uintptr_t>(object);
    if (r.returned == r.after && r.after != 0) {
        auto* data = reinterpret_cast<uintptr_t*>(r.returned + TYPEINFO_PTR_SIZE);
        r.copied = data[1] == 581 && data[2] == 584;
    }
    return nullptr;
}
void CheckAnnotation(unsigned entry)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    AnnotationResult result;
    result.entry = entry;
    auto task = RunCJTask(RunAnnotation, &result);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &taskResult), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "ANNOTATION_HANDLE_TARGET entry=%u called=%d before=%zx after=%zx returned=%zx copied=%d\n",
        entry, result.called, result.before, result.after, result.returned, result.copied);
    GC_EXPECT_TRUE(result.called && result.before != 0 && result.after != 0);
    GC_EXPECT_TRUE(result.before != result.after);
    GC_EXPECT_TRUE(result.returned == result.after && result.copied);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
}
GC_RUNTIME_OTHER_VM_TEST(NativeAnnotationHandle, Parameter) { CheckAnnotation(0); }
GC_RUNTIME_OTHER_VM_TEST(NativeAnnotationHandle, Method) { CheckAnnotation(1); }
GC_RUNTIME_OTHER_VM_TEST(NativeAnnotationHandle, InstanceField) { CheckAnnotation(2); }
GC_RUNTIME_OTHER_VM_TEST(NativeAnnotationHandle, StaticField) { CheckAnnotation(3); }
GC_RUNTIME_OTHER_VM_TEST(NativeAnnotationHandle, Type) { CheckAnnotation(4); }

#include "ObjectManager.inline.h"
#include "Heap/z/zRootsIterator.hpp"
namespace MapleRuntime {
extern "C" void* MCC_ApplyCJStaticMethod(MethodInfo*, void*, TypeInfo*);
}
namespace {
void CollectSparsePages()
{
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(4096 - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    U64 roots[3]{};
    size_t count = 0;
    ZPage* previous = nullptr;
    for (size_t i = 0; i < 4 * ZPageSizeSmall / 4096 && count < 3; ++i) {
        auto* object = MCC_NewObject(type, 4096);
        auto* page = Heap::page(reinterpret_cast<uintptr_t>(object));
        if (page != previous) {
            roots[count++] = Heap::GetHeap().RegisterExportRoot(object);
            previous = page;
        }
    }
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
    for (size_t i = 0; i < count; ++i) { Heap::GetHeap().RemoveExportObject(roots[i]); }
}
struct ArgumentResult {
    TypeInfo* argumentType = nullptr;
    TypeInfo* receiverType = nullptr;
    uintptr_t receiverBefore = 0;
    uintptr_t receiverAfter = 0;
    uintptr_t argumentBefore = 0;
    uintptr_t argumentAfter = 0;
    uintptr_t returned = 0;
    U64 sourceRoot = 0;
    bool called = false;
    bool argumentValue = false;
    bool retainedValue = false;
};
thread_local ArgumentResult* argumentResult;
extern "C" void InitializeReflectedReceiver(MObject* receiver, void* argument, TypeInfo*)
{
    auto& r = *argumentResult;
    r.called = true;
    r.receiverBefore = reinterpret_cast<uintptr_t>(receiver);
    r.argumentBefore = argument == nullptr ? 0 : reinterpret_cast<uintptr_t>(argument) - TYPEINFO_PTR_SIZE;
    if (receiver != nullptr) { receiver->Store<U64>(TYPEINFO_PTR_SIZE, 584); }
    if (argument != nullptr) { r.argumentValue = static_cast<U64*>(argument)[1] == 581; }
    CollectSparsePages();
    auto* source = Heap::GetHeap().GetExportObject(r.sourceRoot);
    Mutator::GetMutator()->VisitMutatorRoots([&](RootSlot& root) {
        auto* object = to_object(safe(root.LoadPlain()));
        if (object == nullptr) { return; }
        if (object->GetTypeInfo() == r.receiverType) {
            r.receiverAfter = reinterpret_cast<uintptr_t>(object);
        }
        if (object != source && object->GetTypeInfo() == r.argumentType) {
            r.argumentAfter = reinterpret_cast<uintptr_t>(object);
            r.retainedValue = reinterpret_cast<U64*>(object)[2] == 581;
        }
    });
}
void* RunArguments(void* context)
{
    auto& r = *static_cast<ArgumentResult*>(context);
    argumentResult = &r;
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    HandleMark mark(*mutator);
    alignas(TypeInfo) static unsigned char storage[4][sizeof(TypeInfo)]{};
    TypeInfo* type[4];
    for (unsigned i = 0; i < 4; ++i) {
        type[i] = reinterpret_cast<TypeInfo*>(storage[i]);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage[i]), sizeof(TypeInfo));
    }
    type[0]->SetType(TypeKind::TYPE_KIND_STRUCT); type[0]->SetInstanceSize(16);
    type[0]->SetFlagHasRefField();
    GCTib bitmap{}; bitmap.tag = SIGN_BIT | 1; type[0]->SetGCTib(bitmap);
    type[1]->SetType(TypeKind::TYPE_KIND_CLASS); type[1]->SetInstanceSize(8);
    type[2]->SetType(TypeKind::TYPE_KIND_UNIT); type[2]->SetInstanceSize(0);
    type[3]->SetType(TypeKind::TYPE_KIND_RAWARRAY); type[3]->SetComponentTypeInfo(type[1]);
    r.argumentType = type[0]; r.receiverType = type[1];
    auto* source = MCC_NewObject(type[0], 24);
    source->Store<U64>(TYPEINFO_PTR_SIZE + sizeof(Uptr), 581);
    r.sourceRoot = Heap::GetHeap().RegisterExportRoot(source);
    auto* array = ObjectManager::NewObjArray(1, type[3]);
    array->SetRefElement(0, source);
    ParameterInfo parameter{}; parameter.SetType(type[0]);
    MethodInfo method{};
    method.SetMethodName("init");
    method.SetActualParameterInfos(reinterpret_cast<Uptr>(&parameter));
    method.SetDeclaringTypeInfo(type[1]);
    auto set = [&](size_t offset, auto value) {
        std::memcpy(reinterpret_cast<char*>(&method) + offset, &value, sizeof(value));
    };
    set(12, U16(1)); set(16, reinterpret_cast<Uptr>(&InitializeReflectedReceiver)); set(24, type[2]);
    CJArray args{};
    StorePlain(RootSlotAt(&args.rawPtr), from_object(array)); args.length = 1;
    r.returned = reinterpret_cast<uintptr_t>(MCC_ApplyCJStaticMethod(&method, &args, nullptr));
    Heap::GetHeap().RemoveExportObject(r.sourceRoot);
    return nullptr;
}
}
GC_RUNTIME_OTHER_VM_TEST(NativeArgumentHandle, CallbackRetainsReceiverAndArguments)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ArgumentResult result;
    auto task = RunCJTask(RunArguments, &result);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &taskResult), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "ARGUMENT_HANDLE_TARGET called=%d receiver=%zx/%zx returned=%zx argument=%zx/%zx value=%d retained=%d\n",
        result.called, result.receiverBefore, result.receiverAfter, result.returned,
        result.argumentBefore, result.argumentAfter, result.argumentValue, result.retainedValue);
    GC_EXPECT_TRUE(result.called && result.argumentBefore != 0 && result.argumentAfter != 0 &&
        result.argumentBefore != result.argumentAfter && result.argumentValue && result.retainedValue);
    GC_EXPECT_TRUE(result.receiverBefore != 0 && result.receiverAfter != 0 &&
        result.receiverBefore != result.receiverAfter && result.returned == result.receiverAfter);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
struct ReturnResult { uintptr_t before = 0; uintptr_t after = 0; uintptr_t encoded = 0; U64 marker = 0; };
thread_local ReturnResult* returnResult;
extern "C" void ReturnReflectedStruct(uintptr_t* output, TypeInfo*)
{
    auto& r = *returnResult;
    auto* mutator = Mutator::GetMutator();
    HandleMark mark(*mutator);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS); type->SetInstanceSize(8);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    Handle value(mutator, MCC_NewObject(type, 16));
    r.before = reinterpret_cast<uintptr_t>(value());
    CollectSparsePages();
    r.after = reinterpret_cast<uintptr_t>(value());
    // This is the callee's actual plain sret value, handed to the product boxer.
    output[0] = r.after; output[1] = 584;
}
void* RunReturn(void* context)
{
    auto& r = *static_cast<ReturnResult*>(context);
    returnResult = &r;
    Mutator::GetMutator()->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_STRUCT); type->SetInstanceSize(16); type->SetFlagHasRefField();
    GCTib bitmap{}; bitmap.tag = SIGN_BIT | 1; type->SetGCTib(bitmap);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    MethodInfo method{}; method.SetMethodName("return_value");
    auto set = [&](size_t offset, auto value) {
        std::memcpy(reinterpret_cast<char*>(&method) + offset, &value, sizeof(value));
    };
    set(8, U32(MODIFIER_STATIC | MODIFIER_HAS_SRET0));
    set(16, reinterpret_cast<Uptr>(&ReturnReflectedStruct)); set(24, type);
    auto* result = static_cast<MObject*>(MCC_ApplyCJStaticMethod(&method, nullptr, nullptr));
    r.encoded = raw(result->GetRefField(TYPEINFO_PTR_SIZE).GetFieldValue());
    r.marker = result->Load<U64>(TYPEINFO_PTR_SIZE + sizeof(Uptr));
    return nullptr;
}
}
GC_RUNTIME_OTHER_VM_TEST(NativeReturnHandle, HeaderlessResultAfterCollection)
{
    RuntimeParam param{}; param.heapParam.heapSize = 512 * 1024; param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ReturnResult result;
    auto task = RunCJTask(RunReturn, &result);
    void* value = nullptr; GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK); ReleaseHandle(task);
    const uintptr_t expected = raw(ZAddress::store_good(to_zaddress(result.after)));
    std::fprintf(stderr, "RETURN_HANDLE_TARGET before=%zx after=%zx encoded=%zx expected=%zx marker=%llu\n",
        result.before, result.after, result.encoded, expected, (unsigned long long)result.marker);
    GC_EXPECT_TRUE(result.before != 0 && result.before != result.after && result.encoded == expected && result.marker == 584);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
struct StableHandleResult { uintptr_t before = 0; uintptr_t after = 0; };
void* GrowHandleArea(void* context)
{
    auto& result = *static_cast<StableHandleResult*>(context);
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    HandleMark mark(*mutator);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(sizeof(Uptr));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    auto* object = MCC_NewObject(type, 2 * sizeof(Uptr));
    Handle first(mutator, object);
    result.before = reinterpret_cast<uintptr_t>(first());
    // Cross more than one storage block without changing the first Handle.
    for (unsigned i = 0; i < 128; ++i) { Handle next(mutator, object); }
    result.after = reinterpret_cast<uintptr_t>(first());
    return nullptr;
}
}
GC_RUNTIME_OTHER_VM_TEST(NativeHandle, SlotsStayStableAcrossGrowth)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    StableHandleResult result;
    auto task = RunCJTask(GrowHandleArea, &result);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "HANDLE_SLOT_TARGET before=%zx after=%zx added=128\n", result.before, result.after);
    GC_EXPECT_TRUE(result.before != 0 && result.before == result.after);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
