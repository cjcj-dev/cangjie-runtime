// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/Handle.h"
#include "Heap/z/zCrossVM.hpp"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "Mutator/Mutator.inline.h"
#include "TypeInfoManager.h"
#include "ObjectModel/MObject.h"
#include <cstring>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo*, MSize);
// Observation through the existing friend; all production and consumption is
// performed by the real collector, with no manually populated root carriers.
class RelocationReceiptTest {
public:
    static bool DiscoveredIdentity(BaseObject* expected)
    {
        auto& cross = Heap::GetHeap().cross_vm();
        std::lock_guard<std::mutex> lock(cross.externMtx);
        for (const auto& entry : Heap::GetHeap().old().discoveredExternObjects) {
            if (entry.first.object != expected) continue;
            const bool keyGood = ZPointer::is_load_good(ZAddress::color(zaddress::null, entry.first.color));
            const bool valueGood = entry.second.size() == 1 &&
                entry.second.front().object == expected &&
                ZPointer::is_load_good(ZAddress::color(zaddress::null, entry.second.front().color));
            std::fprintf(stderr, "VALUE_ROOT_IDENTITY_TARGET expected=%p key=%p key_current=%d value_current=%d\n",
                expected, entry.first.object, keyGood, valueGood);
            return keyGood && valueGood;
        }
        std::fprintf(stderr, "VALUE_ROOT_IDENTITY_TARGET expected=%p missing=1\n", expected);
        return false;
    }
    static bool DiscoveredOwnership(BaseObject* expected)
    {
        const auto& discovered = Heap::GetHeap().old().discoveredExternObjects;
        auto entry = discovered.find(expected);
        return entry != discovered.end() && entry->second.size() == 1 &&
            entry->second.front().object == expected;
    }

    static bool CycleHandoffIdentity(BaseObject* expected)
    {
        auto& cross = Heap::GetHeap().cross_vm();
        std::lock_guard<std::mutex> lock(cross.cycleWorkStackMtx);
        const auto& discovered = Heap::GetHeap().old().discoveredExternObjects;
        auto entry = cross.cycleRefWorkStack.find(expected);
        const bool handedOff = entry != cross.cycleRefWorkStack.end() &&
            entry->second.size() == 1 && entry->second.front().object == expected;
        std::fprintf(stderr, "DISCOVERED_OWNER_HANDOFF empty=%d handed_off=%d\n",
            discovered.empty(), handedOff);
        return discovered.empty() && handedOff;
    }

};
}
namespace {
// An assertion must not leave the product collector parked while the harness
// stops GC workers. Cancellation is the ordinary product shutdown protocol.
struct BreakpointFailureCleanup {
    ~BreakpointFailureCleanup()
    {
        if (std::uncaught_exceptions() != 0) {
            ZAbort::abort();
            ConcurrentGCBreakpoints::ReleaseControl();
        }
    }
};
void* AllocateExportForeignRoot(void* argument)
{
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_FOREIGN_PROXY);
    type->SetInstanceSize(sizeof(void*));
    type->SetFlagHasRefField();
    GCTib tib{};
    tib.tag = SIGN_BIT | 1;
    type->SetGCTib(tib);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    auto* object = static_cast<BaseObject*>(MCC_NewObject(type, TYPEINFO_PTR_SIZE + sizeof(void*)));
    *static_cast<U64*>(argument) = Heap::GetHeap().RegisterExportRoot(object);
    return nullptr;
}
struct SparseRoot {
    U64 root = 0;
    std::vector<U64> roots;
    uintptr_t from = 0;
    uintptr_t to = 0;
    bool table = false;
    bool fromKey = false;
};
void* AllocateSparseExportRoot(void* argument)
{
    auto& result = *static_cast<SparseRoot*>(argument);
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_FOREIGN_PROXY);
    type->SetInstanceSize(4096 - TYPEINFO_PTR_SIZE);
    type->SetFlagHasRefField();
    GCTib tib{};
    tib.tag = SIGN_BIT | 1;
    type->SetGCTib(tib);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    alignas(TypeInfo) static unsigned char fillerStorage[sizeof(TypeInfo)]{};
    auto* fillerType = reinterpret_cast<TypeInfo*>(fillerStorage);
    fillerType->SetType(TypeKind::TYPE_KIND_CLASS);
    fillerType->SetInstanceSize(4096 - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(fillerStorage), sizeof(fillerStorage));
    {
        HandleMark mark(*mutator);
        std::vector<Handle> objects;
        for (size_t i = 0; i < 4 * ZPageSizeSmall / 4096 - 1; ++i) {
            objects.emplace_back(mutator, static_cast<BaseObject*>(MCC_NewObject(type, 4096)));
        }
        objects.emplace_back(mutator, static_cast<BaseObject*>(MCC_NewObject(type, 4096)));
        Heap::GetHeap().RequestGC(GC_REASON_USER);
        BaseObject* selected = objects.front()();
        ZPage* selectedPage = Heap::page(reinterpret_cast<uintptr_t>(selected));
        for (const auto& handle : objects) {
            BaseObject* object = handle();
            ZPage* page = Heap::page(reinterpret_cast<uintptr_t>(object));
            if (page != selectedPage) {
                result.roots.push_back(Heap::GetHeap().RegisterExportRoot(selected));
                if (result.roots.size() == 1) result.from = reinterpret_cast<uintptr_t>(selected);
                selectedPage = page;
            }
            selected = object;
        }
        result.roots.push_back(Heap::GetHeap().RegisterExportRoot(selected));
        result.root = result.roots.front();
    }
    HandleMark fillMark(*mutator);
    std::vector<Handle> capacityRoots;
    alignas(TypeInfo) static unsigned char largeStorage[sizeof(TypeInfo)]{};
    auto* largeType = reinterpret_cast<TypeInfo*>(largeStorage);
    largeType->SetType(TypeKind::TYPE_KIND_CLASS);
    largeType->SetInstanceSize(ZPageSizeSmall - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(largeStorage), sizeof(largeStorage));
    Heap::GetHeap().EnableGC(false);
    while (Heap::GetHeap().page_allocator().GetUsedBytes() + ZPageSizeSmall <= 32 * 1024 * 1024) {
        capacityRoots.emplace_back(mutator,
            static_cast<BaseObject*>(MCC_NewObject(largeType, ZPageSizeSmall)));
    }
    Heap::GetHeap().EnableGC(true);
    std::fprintf(stderr, "VALUE_ROOT_CAPACITY used=%zu large_roots=%zu\n",
        Heap::GetHeap().page_allocator().GetUsedBytes(), capacityRoots.size());
    Heap::GetHeap().RequestGC(GC_REASON_USER);
    result.to = reinterpret_cast<uintptr_t>(Heap::GetHeap().GetExportObject(result.root));
    auto* forwarding = Heap::GetHeap().old().forwarding_table().get(result.to);
    result.table = forwarding != nullptr;
    result.fromKey = forwarding != nullptr && forwarding->find(result.to) != 0;
    std::fprintf(stderr, "VALUE_ROOT_SPARSE_PREP from=%zx to=%zx table=%d from_key=%d\n",
        result.from, result.to, result.table, result.fromKey);
    mutator->SetManagedContext(true);
    return nullptr;
}
}

// zUncoloredRoot.inline.hpp:62-69: a barrier-produced address retains its
// load-good identity when published in a value root. Stop at the real phase
// boundary so the assertion precedes any subsequent forwarding consumption.
GC_RUNTIME_OTHER_VM_TEST(ZValueRoot, ExportDiscoveryPreservesCurrentIdentity)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    U64 root = 0;
    auto task = RunCJTask(AllocateExportForeignRoot, &root);
    GC_EXPECT_TRUE(task != nullptr);
    void* returned = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &returned), E_OK);
    ReleaseHandle(task);
    ConcurrentGCBreakpoints::AcquireControl();
    BreakpointFailureCleanup cleanup;
    GC_EXPECT_TRUE(ConcurrentGCBreakpoints::RunTo("AFTER CONCURRENT REFERENCE PROCESSING STARTED"));
    auto* expected = Heap::GetHeap().GetExportObject(root);
    GC_EXPECT_TRUE(RelocationReceiptTest::DiscoveredIdentity(expected));
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    Heap::GetHeap().RemoveExportObject(root);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// A real minor cycle changes the remap epoch after old root enumeration.
// The owner is already old, so the young table has no forwarding for it:
// make_load_good must retain that address and publish a current carrier.
GC_RUNTIME_OTHER_VM_TEST(ZValueRoot, YoungFlipBetweenEnumerationAndConsumption)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    U64 root = 0;
    auto task = RunCJTask(AllocateExportForeignRoot, &root);
    GC_EXPECT_TRUE(task != nullptr);
    void* returned = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &returned), E_OK);
    ReleaseHandle(task);
    ConcurrentGCBreakpoints::AcquireControl();
    BreakpointFailureCleanup cleanup;
    GC_EXPECT_TRUE(ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED"));
    const uintptr_t savedColor = g_cjLoadGoodMask;
    BaseObject* before = Heap::GetHeap().GetExportObject(root);
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG);
    const bool colorChanged = !ZPointer::is_load_good(ZAddress::color(zaddress::null, savedColor));
    GC_EXPECT_TRUE(ConcurrentGCBreakpoints::RunTo("AFTER CONCURRENT REFERENCE PROCESSING STARTED"));
    BaseObject* after = Heap::GetHeap().GetExportObject(root);
    const bool currentIdentity = RelocationReceiptTest::DiscoveredIdentity(after);
    std::fprintf(stderr, "VALUE_ROOT_EPOCH_TARGET before=%p after=%p color_changed=%d current_identity=%d\n",
        before, after, colorChanged, currentIdentity);
    GC_EXPECT_TRUE(colorChanged && before == after && currentIdentity);
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    Heap::GetHeap().RemoveExportObject(root);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// In-place relocation under a full 32MiB heap: export roots pinned one per
// sparse old small page must be re-resolved to their current addresses after
// the old relocation set completes (issue #839 regression).
GC_RUNTIME_OTHER_VM_TEST(ZValueRoot, CurrentDestinationOnForwardedPage)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 32 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    SparseRoot root;
    auto task = RunCJTask(AllocateSparseExportRoot, &root);
    GC_EXPECT_TRUE(task != nullptr);
    void* returned = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &returned), E_OK);
    ReleaseHandle(task);
    ConcurrentGCBreakpoints::AcquireControl();
    BreakpointFailureCleanup cleanup;
    GC_EXPECT_TRUE(ConcurrentGCBreakpoints::RunTo("AFTER CONCURRENT REFERENCE PROCESSING STARTED"));
    auto* current = Heap::GetHeap().GetExportObject(root.root);
    const bool identity = RelocationReceiptTest::DiscoveredIdentity(current);
    std::fprintf(stderr, "VALUE_ROOT_DESTINATION_TARGET moved=%d table=%d from_key=%d identity=%d\n",
        root.from != root.to, root.table, root.fromKey, identity);
    // ZGC zGeneration.cpp:205-213: forwardings are dropped at the next old
    // collection's relocation-set reset, so table presence is observational;
    // the invariant is re-resolution (moved, current identity) and that a
    // present forwarding never names the destination as a from-key.
    GC_EXPECT_TRUE(root.from != root.to && identity && !root.fromKey);
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    for (U64 id : root.roots) Heap::GetHeap().RemoveExportObject(id);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// zGeneration.cpp:1228 / zReferenceProcessor.cpp:347-359: only the owner
// generation checks its discovered list at the next cycle's mark start.
GC_RUNTIME_OTHER_VM_TEST(ZValueRoot, MinorPreservesOldDiscoveredOwnership)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    U64 root = 0;
    auto task = RunCJTask(AllocateExportForeignRoot, &root);
    GC_EXPECT_TRUE(task != nullptr);
    void* returned = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &returned), E_OK);
    ReleaseHandle(task);
    ConcurrentGCBreakpoints::AcquireControl();
    BreakpointFailureCleanup cleanup;
    GC_EXPECT_TRUE(ConcurrentGCBreakpoints::RunTo("AFTER CONCURRENT REFERENCE PROCESSING STARTED"));
    BaseObject* before = Heap::GetHeap().GetExportObject(root);
    const bool produced = RelocationReceiptTest::DiscoveredOwnership(before);
    const auto oldSequence = Heap::GetHeap().old().seqnum();
    const auto youngSequence = Heap::GetHeap().young().seqnum();
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG);
    BaseObject* after = Heap::GetHeap().GetExportObject(root);
    const bool preserved = RelocationReceiptTest::DiscoveredOwnership(after);
    const bool minorCompleted = Heap::GetHeap().young().seqnum() > youngSequence &&
        Heap::GetHeap().old().seqnum() == oldSequence;
    std::fprintf(stderr, "DISCOVERED_OWNER_ISOLATION produced=%d preserved=%d minor_completed=%d\n",
        produced, preserved, minorCompleted);
    GC_EXPECT_TRUE(produced && preserved && before == after && minorCompleted);
    ConcurrentGCBreakpoints::RunToIdle();
    const bool consumed = RelocationReceiptTest::CycleHandoffIdentity(Heap::GetHeap().GetExportObject(root));
    GC_EXPECT_TRUE(consumed);
    // A subsequent owner cycle must pass the real mark-start empty check.
    GC_EXPECT_TRUE(ConcurrentGCBreakpoints::RunTo("AFTER CONCURRENT REFERENCE PROCESSING STARTED"));
    const bool rediscovered = RelocationReceiptTest::DiscoveredOwnership(Heap::GetHeap().GetExportObject(root));
    std::fprintf(stderr, "DISCOVERED_OWNER_NEXT_CYCLE rediscovered=%d\n", rediscovered);
    GC_EXPECT_TRUE(rediscovered);
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    Heap::GetHeap().RemoveExportObject(root);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
