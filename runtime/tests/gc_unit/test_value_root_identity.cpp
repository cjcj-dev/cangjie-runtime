// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/Handle.h"
#include "Heap/z/zCrossVM.hpp"
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
        for (const auto& entry : cross.discoveredExternObjects) {
            if (entry.first.object != expected) continue;
            const bool keyGood = entry.first.Stage() == ForwardingStage::IncomingNew;
            const bool valueGood = entry.second.size() == 1 &&
                entry.second.front().object == expected &&
                entry.second.front().Stage() == ForwardingStage::IncomingNew;
            std::fprintf(stderr, "VALUE_ROOT_IDENTITY_TARGET expected=%p key=%p key_current=%d value_current=%d\n",
                expected, entry.first.object, keyGood, valueGood);
            return keyGood && valueGood;
        }
        std::fprintf(stderr, "VALUE_ROOT_IDENTITY_TARGET expected=%p missing=1\n", expected);
        return false;
    }
};
}
namespace {
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
    GC_EXPECT_TRUE(ConcurrentGCBreakpoints::RunTo("AFTER CONCURRENT REFERENCE PROCESSING STARTED"));
    auto* expected = Heap::GetHeap().GetExportObject(root);
    GC_EXPECT_TRUE(RelocationReceiptTest::DiscoveredIdentity(expected));
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    Heap::GetHeap().RemoveExportObject(root);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
