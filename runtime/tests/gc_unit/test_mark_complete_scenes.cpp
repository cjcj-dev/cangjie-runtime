// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Cangjie.h"
#include "CjScheduler.h"
#include "Heap/Heap.h"
#include "Heap/Verify/MarkCompleteVerify.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

#if defined(MRT_GC_UNIT_TESTS)
namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo* klass, MSize size);
}

namespace {
TypeInfo* ProductTypeInfo(TypeKind kind)
{
    alignas(TypeInfo) static unsigned char classStorage[sizeof(TypeInfo)];
    alignas(TypeInfo) static unsigned char weakStorage[sizeof(TypeInfo)];
    unsigned char* storage = kind == TypeKind::TYPE_KIND_WEAKREF_CLASS ? weakStorage : classStorage;
    static bool classInitialized = false;
    static bool weakInitialized = false;
    bool& initialized = kind == TypeKind::TYPE_KIND_WEAKREF_CLASS ? weakInitialized : classInitialized;
    if (!initialized) {
        std::memset(storage, 0, sizeof(TypeInfo));
        TypeInfo* info = reinterpret_cast<TypeInfo*>(storage);
        info->SetType(kind);
        info->SetFlagHasRefField();
        info->SetInstanceSize(sizeof(void*));
        GCTib gctib {};
        gctib.tag = SIGN_BIT | 1;
        info->SetGCTib(gctib);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(storage), sizeof(TypeInfo));
        initialized = true;
    }
    return reinterpret_cast<TypeInfo*>(storage);
}

MSize ProductObjectSize()
{
    return static_cast<MSize>(AlignUp<size_t>(sizeof(void*), 8) + TYPEINFO_PTR_SIZE);
}

void* InstallLiveWeakEdge(void*)
{
    BaseObject* holder = reinterpret_cast<BaseObject*>(
        MCC_NewObject(ProductTypeInfo(TypeKind::TYPE_KIND_WEAKREF_CLASS), ProductObjectSize()));
    BaseObject* referent = reinterpret_cast<BaseObject*>(
        MCC_NewObject(ProductTypeInfo(TypeKind::TYPE_KIND_CLASS), ProductObjectSize()));
    if (holder == nullptr || referent == nullptr) {
        return reinterpret_cast<void*>(1);
    }
    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<uintptr_t>(holder) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(GcUnit::StoreGoodPointer(referent));
    (void)Heap::GetHeap().RegisterExportRoot(holder);
    (void)Heap::GetHeap().RegisterExportRoot(referent);
    return nullptr;
}
} // namespace

// Drive the public full-GC entry in a fresh runtime process. The asserted
// receipt is written by the two product verifier invocations after each scene
// has consumed its heap/root result; no test-local verifier copy is linked.
GC_OTHER_VM_TEST(MarkCompleteScenes, MajorGcRunsStrongThenWeakComplete)
{
#if defined(__linux__)
    GC_EXPECT_EQ(setenv("MRT_GCV2_VERIFY_MARKING", "1", 1), 0);
    GC_EXPECT_EQ(setenv("cjProcessorNum", "1", 1), 0);
    RuntimeParam param {};
    param.heapParam.heapSize = 32 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);

    CJThreadHandle task = RunCJTask(InstallLiveWeakEdge, nullptr);
    GC_EXPECT_TRUE(task != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &taskResult), E_OK);
    ReleaseHandle(task);
    GC_EXPECT_TRUE(taskResult == nullptr);

    MarkCompleteVerify::ResetSceneTestReceipt();
    CJ_MRT_ForceFullGC();
    const MarkCompleteVerify::SceneTestReceipt receipt = MarkCompleteVerify::ReadSceneTestReceipt();
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);

    const bool ordered = receipt.strongOnlyCalls == 1 && receipt.weakCompleteCalls == 1 &&
        receipt.strongOnlyOrdinal != 0 && receipt.strongOnlyOrdinal < receipt.weakCompleteOrdinal;
    std::fprintf(stderr,
                 "TARGET_SCENE_ORDER_ASSERT_EXECUTED strongCalls=%llu weakCalls=%llu strongOrdinal=%llu "
                 "weakOrdinal=%llu ordered=%u\n",
                 static_cast<unsigned long long>(receipt.strongOnlyCalls),
                 static_cast<unsigned long long>(receipt.weakCompleteCalls),
                 static_cast<unsigned long long>(receipt.strongOnlyOrdinal),
                 static_cast<unsigned long long>(receipt.weakCompleteOrdinal), ordered ? 1u : 0u);
    GC_EXPECT_TRUE(ordered);

    const bool policy = receipt.strongOnlyIncludedWeak == 0 && receipt.weakCompleteIncludedWeak == 1 &&
        receipt.strongOnlyRootsSeen >= 2 && receipt.weakCompleteRootsSeen >= 2 && receipt.weakEdgesSeen >= 1;
    std::fprintf(stderr,
                 "TARGET_SCENE_POLICY_ASSERT_EXECUTED strongIncludesWeak=%llu weakIncludesWeak=%llu policy=%u "
                 "strongRoots=%llu weakRoots=%llu weakRootFamily=%llu weakEdges=%llu\n",
                 static_cast<unsigned long long>(receipt.strongOnlyIncludedWeak),
                 static_cast<unsigned long long>(receipt.weakCompleteIncludedWeak), policy ? 1u : 0u,
                 static_cast<unsigned long long>(receipt.strongOnlyRootsSeen),
                 static_cast<unsigned long long>(receipt.weakCompleteRootsSeen),
                 static_cast<unsigned long long>(receipt.weakRootsSeen),
                 static_cast<unsigned long long>(receipt.weakEdgesSeen));
    GC_EXPECT_TRUE(policy);
#else
    GC_EXPECT_TRUE(true);
#endif
}

GC_OTHER_VM_TEST(MarkCompleteScenes, ReleaseDefaultLeavesSceneVerificationOff)
{
#if defined(__linux__)
    GC_EXPECT_EQ(unsetenv("MRT_GCV2_VERIFY_MARKING"), 0);
    GC_EXPECT_EQ(unsetenv("MRT_GCV2_MARKCOMPLETE"), 0);
    GC_EXPECT_EQ(setenv("cjProcessorNum", "1", 1), 0);
    RuntimeParam param {};
    param.heapParam.heapSize = 32 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);

    MarkCompleteVerify::ResetSceneTestReceipt();
    CJ_MRT_ForceFullGC();
    const MarkCompleteVerify::SceneTestReceipt receipt = MarkCompleteVerify::ReadSceneTestReceipt();
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);

    const bool disabled = receipt.strongOnlyCalls == 0 && receipt.weakCompleteCalls == 0;
    std::fprintf(stderr,
                 "TARGET_RELEASE_DEFAULT_ASSERT_EXECUTED strongCalls=%llu weakCalls=%llu disabled=%u\n",
                 static_cast<unsigned long long>(receipt.strongOnlyCalls),
                 static_cast<unsigned long long>(receipt.weakCompleteCalls), disabled ? 1u : 0u);
    GC_EXPECT_TRUE(disabled);
#else
    GC_EXPECT_TRUE(true);
#endif
}
#endif
