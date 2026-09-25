// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/z/zCrossVM.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <list>
#include <cstring>
#include <vector>

#include "Heap/z/zBarrier.hpp"

#include "Cangjie.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zObjectAllocator.hpp"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"
#include "gc_unittest.hpp"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zForwarding.hpp"

#include "Heap/z/zAccess.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" int CJ_ScheduleManagerInit();

namespace MapleRuntime {
class ZGenerationRootTest {
public:
    static void Seed(Heap& collector, BaseObject* object)
    {
        std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().cycleWorkStackMtx);
        Heap::GetHeap().cross_vm().cycleRefWorkStack.emplace(ValueRoot(object),
                                            ValueRootList{});
    }

    static void Clear(Heap& collector)
    {
        std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().cycleWorkStackMtx);
        Heap::GetHeap().cross_vm().cycleRefWorkStack.clear();
    }
    static void PostResolveCycleTask(Heap& collector) { Heap::GetHeap().cross_vm().PostResolveCycleTask(); }
};
} // namespace MapleRuntime

namespace {
std::atomic<unsigned> gPosted{0};
std::atomic<bool> gMajorRootObserved{false};
void* gTask = nullptr;
bool gPostedAtMarkComplete = false;
bool gPostedAtRelocate = false;

bool RecordPost(void* task)
{
    gTask = task;
    auto* old = ZGeneration::old();
    gPostedAtMarkComplete = old != nullptr && old->is_phase_mark_complete();
    gPostedAtRelocate = old != nullptr && old->is_phase_relocate();
    gPosted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool NoHigherPriorityTask()
{
    return false;
}

void ExpectPostState(const char* test, unsigned expected)
{
    const unsigned posted = gPosted.load(std::memory_order_relaxed);
    const unsigned taskNonNull = gTask != nullptr ? 1U : 0U;
    std::printf("OHOS_HOST_ASSERT_REACHED test=%s posted=%u task_nonnull=%u expected=%u\n",
                test, posted, taskNonNull, expected);
    std::fflush(stdout);
    GC_EXPECT_EQ(posted, expected);
    GC_EXPECT_EQ(taskNonNull, expected);
}

void* RunMajorCycle(void*)
{
    // ZHeap owns both generations (zHeap.cpp:60-70); a major request runs
    // its young prelude before the old body (zDriver.cpp:443-451). Use the
    // initialized heap collector and driver instead of a second collector.
    auto& collector = static_cast<Heap&>(Heap::GetHeap());
    alignas(TypeInfo) static unsigned char typeStorage[sizeof(TypeInfo)] {};
    auto* type = reinterpret_cast<TypeInfo*>(typeStorage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(sizeof(uint64_t));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(typeStorage), sizeof(typeStorage));
    auto* object = reinterpret_cast<BaseObject*>(collector.object_allocator().alloc_for_relocation(16, PageAge::old));
    object->SetClassInfo(type);
    // The ZGC breakpoint exposes the completed root+follow result before
    // mark-end and relocation (zGeneration.cpp:1086-1092).
    ConcurrentGCBreakpoints::AcquireControl();
    const bool started = ConcurrentGCBreakpoints::RunTo("AFTER MARKING STARTED");
    ZGenerationRootTest::Seed(collector, object);
    auto* page = Heap::page(reinterpret_cast<MAddress>(object));
    std::printf("OHOS_HOST_ROOT_INPUT generation=%u allocating=%u marked_before=%u\n",
                static_cast<unsigned>(page->generation_id()), page->IsAllocating(),
                page->is_object_strongly_live(from_object(object)));
    const bool reached = ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED") && started;
    BaseObject* current = object;
    const bool found = reached && Heap::page(reinterpret_cast<MAddress>(current))->is_object_strongly_live(from_object(current));
    gMajorRootObserved.store(found, std::memory_order_relaxed);
    std::printf("OHOS_HOST_ROOT_RESULT current=%p found=%u\n",
                static_cast<void*>(current), static_cast<unsigned>(found));
    std::fflush(stdout);
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    ZGenerationRootTest::Clear(collector);
    return nullptr;
}
// Managed object payload ABI from zCrossVM.cpp:56-85 and
// zRootsIterator.hpp:188-193. These are heap objects, not copies of the
// product resolver or handler lookup. The host N2C stub passes its first two
// arguments in rdi/rsi (N2CStub.S:329-341, 182-198).
constexpr size_t kPayload = sizeof(BaseObject);
U64 gExportHandle = 0;
unsigned gHandlerCalls = 0;
bool gHandlerArguments = false;
bool gHandlerRoots = false;
bool gOwnerInactive = false;
bool gRelocatedOwner = false;
bool gRelocatedProxy = false;
bool gFromPageReleased = false;

void ObserveHandler(BaseObject* owner, BaseObject* proxy)
{
    auto& heap = Heap::GetHeap();
    BaseObject* expectedOwner = heap.GetExportObject(gExportHandle);
    BaseObject* expectedProxy = expectedOwner == nullptr ? nullptr :
        HeapAccess<>::oop_load(&(expectedOwner->GetRefField<>(kPayload + sizeof(uint64_t))));
    ++gHandlerCalls;
    gHandlerArguments = owner == expectedOwner && proxy == expectedProxy;
    std::vector<BaseObject*> roots;
    heap.cross_vm().VisitSurrectedExportRoots([&](BaseObject* object) { roots.push_back(object); });
    gHandlerRoots = roots.size() == 2 && roots[0] == owner && roots[1] == proxy;
    std::printf("OHOS_HANDLER_TARGET_REACHED arguments=%u roots=%u count=%u\n",
                gHandlerArguments, gHandlerRoots, gHandlerCalls);
    std::fflush(stdout);
}

void* RunHandlerChain(void* argument)
{
    auto& heap = Heap::GetHeap();
    alignas(TypeInfo) static unsigned char metadata[4][sizeof(TypeInfo)] {};
    BaseObject* objects[4] {};
    for (size_t i = 0; i < 4; ++i) {
        auto* type = reinterpret_cast<TypeInfo*>(metadata[i]);
        type->SetType(i == 1 ? TypeKind::TYPE_KIND_FOREIGN_PROXY : TypeKind::TYPE_KIND_CLASS);
        type->SetInstanceSize(i == 0 ? 16 : 8);
        if (i < 3) {
            type->SetFlagHasRefField();
            GCTib bitmap {};
            bitmap.tag = SIGN_BIT | (i == 0 ? 2 : 1);
            type->SetGCTib(bitmap);
        }
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(metadata[i]), sizeof(TypeInfo));
        const size_t size = kPayload + (i == 0 ? 16 : 8);
        objects[i] = reinterpret_cast<BaseObject*>(heap.object_allocator().alloc_for_relocation(size, PageAge::old));
        std::memset(objects[i], 0, size);
        objects[i]->SetClassInfo(type);
    }
    // ExportObject(id, foreignProxy) -> CJForeignProxy(context) ->
    // CJInteropContext(cjFunc) -> CJFunc(handler).
    for (size_t i = 0; i < 3; ++i) {
        HeapAccess<>::oop_store(&(objects[i]->GetRefField<>(kPayload + (i == 0 ? sizeof(uint64_t) : 0))), objects[i + 1]);
    }
    const CrossRefHandler handler = &ObserveHandler;
    std::memcpy(reinterpret_cast<char*>(objects[3]) + kPayload, &handler, sizeof(handler));
    gExportHandle = heap.RegisterExportRoot(objects[0]);
    const U32 index = ExportRootTable::ExportHandleIndex(gExportHandle);
    std::memcpy(reinterpret_cast<char*>(objects[0]) + kPayload, &index, sizeof(index));

    const uintptr_t ownerBefore = reinterpret_cast<uintptr_t>(objects[0]);
    const uintptr_t proxyBefore = reinterpret_cast<uintptr_t>(objects[1]);
    if (argument != nullptr) {
        // Fill multiple old pages with unreachable ordinary objects so the
        // real major selector sees sparse pages and relocates the live chain.
        alignas(TypeInfo) static unsigned char fillerMetadata[sizeof(TypeInfo)] {};
        auto* filler = reinterpret_cast<TypeInfo*>(fillerMetadata);
        filler->SetType(TypeKind::TYPE_KIND_CLASS);
        filler->SetInstanceSize(4096 - kPayload);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(filler), sizeof(TypeInfo));
        for (size_t n = 0; n < 4 * ZPageSizeSmall / 4096; ++n) {
            auto* dead = reinterpret_cast<BaseObject*>(
                heap.object_allocator().alloc_for_relocation(4096, PageAge::old));
            dead->SetClassInfo(filler);
        }
    }

    // The real major cycle owns export enumeration, foreign discovery,
    // PrepareCycleRef, and task posting. Do not seed its intermediate maps.
    ConcurrentGCBreakpoints::AcquireControl();
    const bool started = ConcurrentGCBreakpoints::RunTo("AFTER MARKING STARTED");
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    if (argument != nullptr) {
        BaseObject* currentOwner = heap.GetExportObject(gExportHandle);
        BaseObject* currentProxy = currentOwner == nullptr ? nullptr :
            HeapAccess<>::oop_load(&(currentOwner->GetRefField<>(kPayload + sizeof(uint64_t))));
        gRelocatedOwner = reinterpret_cast<uintptr_t>(currentOwner) != ownerBefore;
        gRelocatedProxy = reinterpret_cast<uintptr_t>(currentProxy) != proxyBefore;
        auto* forwarding = heap.old().forwarding_table().get(ownerBefore);
        gFromPageReleased = forwarding != nullptr && forwarding->page() == nullptr;
        std::printf("OHOS_RELOCATE_INPUT owner_moved=%u proxy_moved=%u from_released=%u\n",
                    gRelocatedOwner, gRelocatedProxy, gFromPageReleased);
    }
    void* postedTask = gTask;
    if (started && postedTask != nullptr) {
        reinterpret_cast<void(*)()>(postedTask)();
    }
    BaseObject* current = heap.GetExportObject(gExportHandle);
    gOwnerInactive = current != nullptr && !heap.CheckExportObjState(gExportHandle, current);
    ZGenerationRootTest::Clear(heap);
    heap.RemoveExportObject(gExportHandle);
    return nullptr;
}
} // namespace

GC_RUNTIME_TEST(OHOSCycle, HandlerChainThroughMajorEntry)
{
    RuntimeParam param {};
    param.heapParam.heapSize = 32 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    RegisterEventHandlerCallbacks(&RecordPost, &NoHigherPriorityTask);
    CJThreadHandle handle = RunCJTask(RunHandlerChain, nullptr);
    GC_EXPECT_TRUE(handle != nullptr);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &result), E_OK);
    ReleaseHandle(handle);
    std::printf("OHOS_HOST_ASSERT_REACHED test=OHOSCycle.HandlerChainThroughMajorEntry "
                "calls=%u arguments=%u roots=%u inactive=%u\n",
                gHandlerCalls, gHandlerArguments, gHandlerRoots, gOwnerInactive);
    std::fflush(stdout);
    // Always reach these target assertions even when a producer cut prevents
    // posting/handler delivery. No earlier presence assertion masks them.
    std::printf("OHOS_POST_PHASE_ASSERT_REACHED mark_complete=%u relocate=%u posted=%u\n",
                gPostedAtMarkComplete, gPostedAtRelocate, gPosted.load());
    std::fflush(stdout);
    GC_EXPECT_TRUE(gPostedAtMarkComplete);
    GC_EXPECT_FALSE(gPostedAtRelocate);
    GC_EXPECT_EQ(gHandlerCalls, 1U);
    GC_EXPECT_TRUE(gHandlerArguments);
    GC_EXPECT_TRUE(gHandlerRoots);
    GC_EXPECT_TRUE(gOwnerInactive);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_TEST(OHOSCycle, HandlerReceivesCurrentRootsAfterRelocate)
{
    RuntimeParam param {};
    param.heapParam.heapSize = 32 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    RegisterEventHandlerCallbacks(&RecordPost, &NoHigherPriorityTask);
    bool relocate = true;
    CJThreadHandle handle = RunCJTask(RunHandlerChain, &relocate);
    GC_EXPECT_TRUE(handle != nullptr);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &result), E_OK);
    ReleaseHandle(handle);
    std::printf("OHOS_HOST_ASSERT_REACHED test=OHOSCycle.HandlerReceivesCurrentRootsAfterRelocate "
                "calls=%u arguments=%u roots=%u inactive=%u owner_moved=%u proxy_moved=%u from_released=%u\n",
                gHandlerCalls, gHandlerArguments, gHandlerRoots, gOwnerInactive,
                gRelocatedOwner, gRelocatedProxy, gFromPageReleased);
    std::fflush(stdout);
    GC_EXPECT_TRUE(gHandlerArguments);
    GC_EXPECT_EQ(gHandlerCalls, 1U);
    GC_EXPECT_TRUE(gHandlerRoots);
    GC_EXPECT_TRUE(gOwnerInactive);
    GC_EXPECT_TRUE(gRelocatedOwner);
    GC_EXPECT_TRUE(gRelocatedProxy);
    GC_EXPECT_TRUE(gFromPageReleased);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_TEST(OHOSCycle, PostResolvePostsProductTask)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    RegisterEventHandlerCallbacks(&RecordPost, &NoHigherPriorityTask);
    Heap& collector = Heap::GetHeap();
    ZGenerationRootTest::Seed(collector, reinterpret_cast<BaseObject*>(uintptr_t{1}));
    ZGenerationRootTest::PostResolveCycleTask(collector);
    ZGenerationRootTest::Clear(collector);
    ExpectPostState("OHOSCycle.PostResolvePostsProductTask", 1U);
}

GC_TEST(OHOSCycle, EmptyWorkDoesNotPost)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    RegisterEventHandlerCallbacks(&RecordPost, &NoHigherPriorityTask);
    Heap& collector = Heap::GetHeap();
    ZGenerationRootTest::Clear(collector);
    ZGenerationRootTest::PostResolveCycleTask(collector);
    ExpectPostState("OHOSCycle.EmptyWorkDoesNotPost", 0U);
}

GC_RUNTIME_TEST(OHOSCycle, MajorEntryPostsResolveTask)
{
    RuntimeParam param {};
    param.heapParam.heapSize = 32 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    RegisterEventHandlerCallbacks(&RecordPost, &NoHigherPriorityTask);
    CJThreadHandle handle = RunCJTask(RunMajorCycle, nullptr);
    GC_EXPECT_TRUE(handle != nullptr);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &result), E_OK);
    ReleaseHandle(handle);
    ExpectPostState("OHOSCycle.MajorEntryPostsResolveTask", 1U);
    GC_EXPECT_TRUE(gMajorRootObserved.load(std::memory_order_relaxed));
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
