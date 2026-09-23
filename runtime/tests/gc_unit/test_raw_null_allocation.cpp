// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "gc_generation_test.hpp"
#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Heap/z/zBarrierSet.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/MObject.h"
#include <sys/wait.h>
#include <unistd.h>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo*, MSize);
extern "C" ObjRef MCC_NewFinalizer(const TypeInfo*, MSize);
extern "C" ObjRef MCC_NewPinnedObject(const TypeInfo*, MSize, bool);
extern "C" ArrayRef MCC_NewArray(const TypeInfo*, MIndex);
extern "C" ArrayRef MCC_NewObjArray(const TypeInfo*, MIndex);
extern "C" ArrayRef MCC_NewArray8(const TypeInfo*, MIndex);
extern "C" ArrayRef MCC_NewArray16(const TypeInfo*, MIndex);
extern "C" ArrayRef MCC_NewArray32(const TypeInfo*, MIndex);
extern "C" ArrayRef MCC_NewArray64(const TypeInfo*, MIndex);
}
namespace {
// Drive the generation's real selection entry, including installation and
// promotion tracking. Only the heap/mark state is fixture input.
void CheckSelection(bool medium, bool promote, uint32_t workers)
{
    CreateStandaloneHeap(medium ? 4 : 2);
    if (medium) {
        ZHeuristics::set_max_heap_size(128 * 1024 * 1024);
        ZHeuristics::set_medium_page_size();
    }
    const size_t size = medium ? ZPageSizeMediumMax : ZPageSizeSmall;
    const size_t objectSize = medium ? ZObjectAlignmentMedium : 24;
    const PageAge age = promote ? PageAge::survivor1 : PageAge::eden;
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    auto& generation = Heap::GetHeap().young();
    generation.InitializeWorkers(workers);
    generation.Workers()->set_active_workers(workers);
    GenerationSequenceFixture::Advance(generation);
    ZGenerationTest::SetTenuringThreshold(generation, promote ? 1u : 15u);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(objectSize - TYPEINFO_PTR_SIZE);
    type->SetAlign(8);
    GCTib tib{};
    tib.tag = SIGN_BIT;
    type->SetGCTib(tib);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZPage* pages[2];
    BaseObject* objects[2];
    ZRelocationSetSelector selector(ZFragmentationLimit);
    for (unsigned i = 0; i < 2; ++i) {
        pages[i] = Heap::alloc_page(size, medium ? ZPageType::medium : ZPageType::small,
                                    false, false, age, flags);
        GC_EXPECT_TRUE(pages[i] != nullptr);
        objects[i] = reinterpret_cast<BaseObject*>(pages[i]->alloc_object(objectSize));
        objects[i]->SetClassInfo(type);
        GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(pages[i], objects[i]));
        GC_EXPECT_TRUE(pages[i]->allows_raw_null());
        ZPageTest::MakeRelocatable(*pages[i]);
        selector.register_live_page(pages[i]);
    }
    selector.select();
    generation.relocation_set().install(&selector);
    ZRelocationSetIterator installed(&generation.relocation_set());
    for (ZForwarding* owner; installed.next(&owner);) {
        generation.forwarding_table().insert(owner);
    }
    for (unsigned i = 0; i < 2; ++i) {
        const bool allows = pages[i]->allows_raw_null();
        const auto* forwarding = generation.forwarding_table().get(reinterpret_cast<MAddress>(objects[i]));
        const bool installedPage = forwarding != nullptr;
        std::fprintf(stderr, "RAW_NULL_INSTALL_TARGET medium=%d promote=%d workers=%u page=%u allows=%d installed=%d\n",
                     medium, promote, workers, i, allows, installedPage);
        GC_EXPECT_EQ(allows, !promote);
        GC_EXPECT_TRUE(installedPage);
    }
}

struct ExitCase { unsigned entry; bool reject; };
void* AllocateAtExit(void* opaque)
{
    const auto& input = *static_cast<ExitCase*>(opaque);
    alignas(TypeInfo) static unsigned char objectStorage[sizeof(TypeInfo)]{};
    alignas(TypeInfo) static unsigned char elementStorage[sizeof(TypeInfo)]{};
    alignas(TypeInfo) static unsigned char arrayStorage[sizeof(TypeInfo)]{};
    auto* objectType = reinterpret_cast<TypeInfo*>(objectStorage);
    auto* elementType = reinterpret_cast<TypeInfo*>(elementStorage);
    auto* arrayType = reinterpret_cast<TypeInfo*>(arrayStorage);
    objectType->SetType(TypeKind::TYPE_KIND_CLASS);
    objectType->SetInstanceSize(24 - TYPEINFO_PTR_SIZE);
    objectType->SetAlign(8);
    GCTib tib{}; tib.tag = SIGN_BIT; objectType->SetGCTib(tib);
    const TypeKind kinds[] = {TypeKind::TYPE_KIND_UINT8, TypeKind::TYPE_KIND_UINT16,
                              TypeKind::TYPE_KIND_UINT32, TypeKind::TYPE_KIND_UINT64};
    const unsigned width = input.entry >= 5 ? input.entry - 5 : 0;
    elementType->SetType(input.entry == 4 ? TypeKind::TYPE_KIND_CLASS : kinds[width]);
    elementType->SetInstanceSize(input.entry == 4 ? 8 : 1u << width);
    arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    arrayType->SetComponentTypeInfo(elementType);
    for (auto* storage : {objectStorage, elementStorage, arrayStorage}) {
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(TypeInfo));
    }
    BaseObject* first = MCC_NewObject(objectType, 24);
    ZPage* page = Heap::page(reinterpret_cast<MAddress>(first));
    if (input.reject) {
        // Adversarial entry state: revoke the current allocation page's promise.
        // This uses the product setter, not a test-only hook or interposition.
        page->set_is_relocate_promoted();
    }
    BaseObject* result = nullptr;
    switch (input.entry) {
        case 0: result = MCC_NewObject(objectType, 24); break;
        case 1: result = MCC_NewFinalizer(objectType, 24); break;
        case 2: result = MCC_NewPinnedObject(objectType, 24, false); break;
        case 3: result = MCC_NewArray(arrayType, 2); break;
        case 4: result = MCC_NewObjArray(arrayType, 2); break;
        case 5: result = MCC_NewArray8(arrayType, 2); break;
        case 6: result = MCC_NewArray16(arrayType, 2); break;
        case 7: result = MCC_NewArray32(arrayType, 2); break;
        case 8: result = MCC_NewArray64(arrayType, 2); break;
    }
    ZPage* actual = Heap::page(reinterpret_cast<MAddress>(result));
    std::fprintf(stderr, "RAW_NULL_EXIT_RETURN entry=%u same_page=%d allows=%d\n",
                 input.entry, page == actual, actual->allows_raw_null());
    return reinterpret_cast<void*>(actual->allows_raw_null() ? 0 : 1);
}

void CheckExit(unsigned entry, bool reject)
{
    int fds[2];
    GC_EXPECT_EQ(pipe(fds), 0);
    pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDERR_FILENO) < 0) { _exit(120); }
        close(fds[1]);
        RuntimeParam param{};
        param.heapParam.heapSize = 512 * 1024;
        param.coParam.processorNum = 1;
        if (InitCJRuntime(&param) != E_OK) { _exit(121); }
        ExitCase input{entry, reject};
        auto handle = RunCJTask(AllocateAtExit, &input);
        void* result = nullptr;
        if (handle == nullptr || GetTaskRet(handle, &result) != E_OK) { _exit(122); }
        ReleaseHandle(handle);
        // The rejection case must terminate in the product exit check.
        _exit(result == nullptr ? 0 : 123);
    }
    close(fds[1]);
    std::string output;
    char buffer[1024];
    ssize_t count;
    while ((count = read(fds[0], buffer, sizeof(buffer))) > 0) { output.append(buffer, count); }
    close(fds[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    std::fwrite(output.data(), 1, output.size(), stderr);
    const bool target = output.find("allocation exit must allow raw null") != std::string::npos;
    std::fprintf(stderr, "RAW_NULL_EXIT_TARGET entry=%u reject=%d diagnostic=%d status=%d\n",
                 entry, reject, target, status);
    GC_EXPECT_EQ(target, reject);
    if (!reject) { GC_EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0); }
    else { GC_EXPECT_TRUE(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0)); }
}
}
GC_RUNTIME_OTHER_VM_TEST(RawNullInstall, SmallPromotion) { CheckSelection(false, true, 1); }
GC_RUNTIME_OTHER_VM_TEST(RawNullInstall, MediumPromotion) { CheckSelection(true, true, 2); }
GC_RUNTIME_OTHER_VM_TEST(RawNullInstall, YoungControl) { CheckSelection(false, false, 1); }
#define RAW_NULL_EXIT_TEST(Name, Index) \
GC_RUNTIME_OTHER_VM_TEST(RawNullExit, Name##Accept) { CheckExit(Index, false); } \
GC_RUNTIME_OTHER_VM_TEST(RawNullExit, Name##Reject) { CheckExit(Index, true); }
RAW_NULL_EXIT_TEST(Object, 0)
RAW_NULL_EXIT_TEST(Finalizer, 1)
RAW_NULL_EXIT_TEST(Pinned, 2)
RAW_NULL_EXIT_TEST(Array, 3)
RAW_NULL_EXIT_TEST(ObjArray, 4)
RAW_NULL_EXIT_TEST(Array8, 5)
RAW_NULL_EXIT_TEST(Array16, 6)
RAW_NULL_EXIT_TEST(Array32, 7)
RAW_NULL_EXIT_TEST(Array64, 8)
#undef RAW_NULL_EXIT_TEST

GC_COMPONENT_OTHER_VM_TEST(RawNullPage, OldPageRejectsRawNull)
{
    CreateStandaloneHeap(4);
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZPage* young = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, false, PageAge::eden, flags);
    ZPage* old = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, false, PageAge::old, flags);
    GC_EXPECT_TRUE(young != nullptr && old != nullptr);
    const bool youngAllows = young->allows_raw_null();
    const bool oldAllows = old->allows_raw_null();
    std::fprintf(stderr, "RAW_NULL_PAGE_TARGET young=%d old=%d\n", youngAllows, oldAllows);
    GC_EXPECT_TRUE(youngAllows);
    GC_EXPECT_FALSE(oldAllows);
}
