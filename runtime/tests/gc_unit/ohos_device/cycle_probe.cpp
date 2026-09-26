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

#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "Heap/z/zPage.inline.hpp"

#include "Heap/z/zAccess.hpp"

#include "Common/ScopedObjectAccess.h"
using namespace MapleRuntime;
extern "C" bool MRT_NewForeignCJThread();
extern "C" bool MRT_EndForeignCJThread();


namespace {
std::atomic<void*> postedTask{nullptr};
bool RecordPost(void* task)
{
    postedTask.store(task, std::memory_order_release);
    return true;
}
bool NoHigherPriorityTask() { return false; }
constexpr size_t kPayload = sizeof(BaseObject);
U64 gExportHandle = 0;
unsigned gHandlerCalls = 0;
bool gHandlerArguments = false;
bool gHandlerRoots = false;
bool gOwnerInactive = false;

// ResolveCycleRefStub preserves x0 (callee), x1 (owner), x2 (proxy)
// through CJ_MCC_N2CStub on AArch64; see N2CStub.S:255-265,430-441.
void ObserveHandler(void*, BaseObject* owner, BaseObject* proxy)
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
    std::printf("OHOS_DEVICE_HANDLER_RESULT arguments=%u roots=%u count=%u\n",
                gHandlerArguments, gHandlerRoots, gHandlerCalls);
    std::fflush(stdout);
}

void* RunHandlerChain(void*)
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
    const auto handler = &ObserveHandler;
    std::memcpy(reinterpret_cast<char*>(objects[3]) + kPayload, &handler, sizeof(handler));
    gExportHandle = heap.RegisterExportRoot(objects[0]);
    const U32 index = ExportRootTable::ExportHandleIndex(gExportHandle);
    std::memcpy(reinterpret_cast<char*>(objects[0]) + kPayload, &index, sizeof(index));

    // The real major cycle owns export enumeration, foreign discovery,
    // PrepareCycleRef, and task posting. Do not seed its intermediate maps.
    ConcurrentGCBreakpoints::AcquireControl();
    const bool started = ConcurrentGCBreakpoints::RunTo("AFTER MARKING STARTED");
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    void* task = postedTask.load(std::memory_order_acquire);
    if (started && task != nullptr) {
        reinterpret_cast<void(*)()>(task)();
    }
    BaseObject* current = heap.GetExportObject(gExportHandle);
    gOwnerInactive = current != nullptr && !heap.CheckExportObjState(gExportHandle, current);
    heap.RemoveExportObject(gExportHandle);
    return nullptr;
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && std::strcmp(argv[1], "empty") == 0) {
        ZCrossVM vm;
        vm.PostResolveCycleTask();
        std::puts("OHOS_DEVICE_EMPTY_RETURNED");
        return 0;
    }
    RuntimeParam param {};
    param.heapParam.heapSize = 32 * 1024;
    param.coParam.processorNum = 1;
    int rc = InitCJRuntime(&param);
    std::printf("OHOS_DEVICE_INIT rc=%d\n", rc);
    if (rc != E_OK) { return 10; }
    RegisterEventHandlerCallbacks(&RecordPost, &NoHigherPriorityTask);
    // This fixture is native C++, not compiler-emitted managed code. Attach
    // through the real foreign-thread entry instead of ExecuteCangjieStub.
    if (!MRT_NewForeignCJThread()) { return 11; }
    Mutator::GetMutator()->SetManagedContext(false);
    {
        ScopedObjectAccess access;
        RunHandlerChain(nullptr);
    }
    if (!MRT_EndForeignCJThread()) { return 12; }
    // Evaluate all target results even when posting or handler delivery is cut.
    std::printf("OHOS_DEVICE_TARGET_ASSERT calls=%u arguments=%u roots=%u inactive=%u task_rc=%d\n",
                gHandlerCalls, gHandlerArguments, gHandlerRoots, gOwnerInactive, rc);
    const bool ok = rc == E_OK && gHandlerCalls == 1 && gHandlerArguments && gHandlerRoots && gOwnerInactive;
    const int fini = FiniCJRuntime();
    return ok && fini == E_OK ? 0 : 1;
}
