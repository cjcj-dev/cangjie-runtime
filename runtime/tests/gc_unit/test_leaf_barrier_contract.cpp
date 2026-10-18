// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/BaseObject.inline.h"
#include "Common/Handle.h"
#include "Heap/z/zRootsIterator.hpp"
#include "Mutator/Mutator.h"
#include "ObjectModel/MClass.h"
#include "TypeInfoManager.h"
#include <array>
#include <cstring>
#include <cstdio>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo*, MSize);
extern "C" void MCC_WriteStructField(ObjectPtr, MAddress, size_t, MAddress, size_t, GCTib);
extern "C" void MCC_WriteStaticStruct(MAddress, size_t, MAddress, size_t, GCTib);
extern "C" void CJ_MCC_ReadGeneric(ObjectPtr, ObjectPtr, void*, size_t);
extern "C" void CJ_MCC_ReadGenericPayload(void*, ObjectPtr, size_t);
}

namespace {
struct Observation { bool attached = false; bool matches = false; bool staticControl = false; };
void* ExerciseLeafHelpers(void* opaque)
{
    auto& result = *static_cast<Observation*>(opaque);
    Mutator* mutator = Mutator::GetMutator();
    result.attached = mutator != nullptr;
    if (!result.attached) { return nullptr; }
    // The scheduler owns this mutator. This C++ task has no generated stack map.
    mutator->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char metadata[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(metadata);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(512);
    type->SetGCTib(GCTib{});
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(metadata), sizeof(metadata));
    HandleMark roots(*mutator);
    Handle objectRoot(mutator, MCC_NewObject(type, 512 + TYPEINFO_PTR_SIZE));
    Handle destinationRoot(mutator, MCC_NewObject(type, 512 + TYPEINFO_PTR_SIZE));
    ObjRef object = objectRoot();
    ObjRef destination = destinationRoot();
    std::array<unsigned char, 512> input{}, output{};
    for (size_t i = 0; i < input.size(); ++i) { input[i] = static_cast<unsigned char>(i ^ 0x5a); }
    MCC_WriteStructField(object, reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE, input.size(),
                         reinterpret_cast<MAddress>(input.data()), input.size(), GCTib{});
    std::fprintf(stderr, "LEAF_BARRIER_TARGET helper=ReadGenericPayload entered=1 registered=1\n");
    CJ_MCC_ReadGenericPayload(output.data(), object, output.size());
    result.matches = output == input;
    // The global branch with size >= 256 performs native malloc/free. It must
    // remain legal under the leaf verifier; malloc is not managed allocation.
    CJ_MCC_ReadGeneric(destination, reinterpret_cast<ObjectPtr>(uintptr_t(1)), input.data(), input.size());
    CJ_MCC_ReadGenericPayload(output.data(), destination, output.size());
    result.matches &= output == input;
    mutator->SetManagedContext(true);
    return nullptr;
}

void* ExerciseStaticControl(void* opaque)
{
    auto& result = *static_cast<Observation*>(opaque);
    result.attached = Mutator::GetMutator() != nullptr;
    if (!result.attached) { return nullptr; }
    Mutator::GetMutator()->SetManagedContext(false);
    struct Payload { uintptr_t reference; uintptr_t number; } input{0, 0x13561350}, output{};
    GCTib layout{};
    layout.tag = SIGN_BIT | 1;
    // Layout enumeration allocates a native vector; a reference-bearing value
    // copy must remain legal without treating its metadata as a managed object.
    MCC_WriteStaticStruct(reinterpret_cast<MAddress>(&output), sizeof(output),
                         reinterpret_cast<MAddress>(&input), sizeof(input), layout);
    result.staticControl = output.number == input.number;
    Mutator::GetMutator()->SetManagedContext(true);
    return nullptr;
}

Observation RunLeafTask(CJTaskFunc fn)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 128 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.staticGCThreads = true;
    params.gcParam.concGCThreads = 2;
    params.gcParam.concGCThreadsSet = true;
    params.gcParam.youngGCThreads = 2;
    params.gcParam.youngGCThreadsSet = true;
    params.gcParam.oldGCThreads = 2;
    params.gcParam.oldGCThreadsSet = true;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    Observation result;
    auto task = RunCJTask(fn, &result);
    GC_EXPECT_TRUE(task != nullptr);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
    return result;
}
}

GC_RUNTIME_OTHER_VM_TEST(LeafBarrierContract, GenericNativeAllocationAndPayloadCopy)
{
    auto result = RunLeafTask(ExerciseLeafHelpers);
    GC_EXPECT_TRUE(result.attached);
    GC_EXPECT_TRUE(result.matches);
    std::fprintf(stderr, "LEAF_BARRIER_RESULT executed=1 registered=1 bytes_match=1\n");
}

GC_RUNTIME_OTHER_VM_TEST(LeafBarrierContract, ReferenceLayoutNativeAllocationControl)
{
    auto result = RunLeafTask(ExerciseStaticControl);
    GC_EXPECT_TRUE(result.attached);
    GC_EXPECT_TRUE(result.staticControl);
    std::fprintf(stderr, "LEAF_STATIC_CONTROL executed=1 registered=1 bytes_match=1\n");
}
