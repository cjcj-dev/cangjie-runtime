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

namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo* klass, MSize size);
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
void* AllocateGranulePages(void* context)
{
    auto& result = *static_cast<GranuleAllocationResult*>(context);
    alignas(TypeInfo) static unsigned char types[3][sizeof(TypeInfo)];
    result.requested[0] = 32;
    result.requested[1] = ZObjectSizeLimitSmall + 16;
    result.requested[2] = ZObjectSizeLimitMedium + ZGranuleSize + 16;
    result.expected[0] = ZPageSizeSmall;
    result.expected[1] = ZPageSizeMediumMin;
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

GC_OTHER_VM_TEST(ZPageGranule, MutatorAllocatesThreePageSizes)
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
    size_t prepared{0};
    size_t retained{0};
    size_t retired{0};
    size_t receipts{0};
};
void* SelectRealLivePages(void* context)
{
    auto& result = *static_cast<ForwardingSelectionResult*>(context);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)];
    std::memset(storage, 0, sizeof(storage));
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(4096 - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    uintptr_t starts[3]{};
    U64 roots[3]{};
    ZPage* previous = nullptr;
    for (size_t i = 0; i < 4 * ZPageSizeSmall / 4096 && result.roots < 3; ++i) {
        const ObjRef object = MCC_NewObject(type, 4096);
        if (object == nullptr) { return nullptr; }
        ZPage* page = Heap::page(reinterpret_cast<uintptr_t>(object));
        if (page != previous) {
            starts[result.roots] = reinterpret_cast<uintptr_t>(object);
            roots[result.roots] = Heap::GetHeap().RegisterExportRoot(reinterpret_cast<BaseObject*>(object));
            ++result.roots;
            previous = page;
        }
    }
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    // Live objects in three real small pages force a non-empty relocation set.
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
    for (size_t i = 0; i < result.roots; ++i) {
        ZForwarding* forwarding = Heap::GetHeap().young().forwarding_table().get(starts[i]);
        if (forwarding != nullptr) {
            ++result.published;
            result.retired += Heap::page(starts[i]) == nullptr;
            // Consume the retired source through the product remap entry;
            // do not recompile the inline forwarding lookup into the test.
            result.receipts += ZGeneration::young()->relocate_or_remap_object(
                reinterpret_cast<BaseObject*>(starts[i])) == Heap::GetHeap().GetExportObject(roots[i]);
            const auto* view = forwarding->from_page_snapshot();
            result.prepared += view != nullptr && view->livemap != nullptr &&
                               view->topAtStart > starts[i];
        }
        result.retained += Heap::GetHeap().GetExportObject(roots[i]) != nullptr;
        Heap::GetHeap().RemoveExportObject(roots[i]);
    }
    mutator->SetManagedContext(true);
    return nullptr;
}
}

GC_OTHER_VM_TEST(ZForwardingPublication, SelectionPublishesPreparedForwardingOnce)
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
    std::fprintf(stderr, "FORWARDING_SELECTION_TARGET roots=%zu published=%zu prepared=%zu retained=%zu\n",
                 result.roots, result.published, result.prepared, result.retained);
    GC_EXPECT_TRUE(result.published > 0);
    GC_EXPECT_EQ(result.prepared, result.published);
    GC_EXPECT_EQ(result.retained, 3u);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// The product mutator allocates and roots the objects; RequestGC owns selection,
// copying, detach and retirement. No manually installed forwarding is involved.
GC_OTHER_VM_TEST(ZRelocationRetirement, CopiedSourceLeavesPageTable)
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
    std::fprintf(stderr, "SOURCE_RETIREMENT_TARGET selected=%zu retired=%zu receipts=%zu retained=%zu\n",
                 result.published, result.retired, result.receipts, result.retained);
    GC_EXPECT_TRUE(result.published > 0);
    GC_EXPECT_EQ(result.retired, result.published);
    GC_EXPECT_EQ(result.receipts, result.published);
    GC_EXPECT_EQ(result.retained, 3u);
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

GC_OTHER_VM_TEST(ZGenerationPhases, OldCycleFlipsRemapMaskOnce)
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
