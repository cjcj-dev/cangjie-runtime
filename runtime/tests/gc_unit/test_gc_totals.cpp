// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/Handle.h"
#include "Heap/z/zHeap.hpp"
#include "Mutator/Mutator.inline.h"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"
#include <cstdio>

namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo*, MSize);
extern "C" uint64_t MCC_GetGCTimeUs();
extern "C" size_t MCC_GetGCFreedSize();
namespace {
struct Totals {
    bool minor;
    uint64_t time[4]{};
    size_t freed[4]{};
};

void* CollectTotals(void* context)
{
    auto& result = *static_cast<Totals*>(context);
    auto& heap = Heap::GetHeap();
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    constexpr size_t size = 4 * 1024 * 1024;
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(size - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    for (unsigned round = 0; round < 4; ++round) {
        if (round != 0) {
            // Allocate through the product mutator and release the only root.
            // No statistics or collector intermediate values are injected.
            {
                HandleMark roots(*mutator);
                Handle root(mutator, MCC_NewObject(type, size));
            }
            heap.RequestGC(result.minor ? GC_REASON_YOUNG : GC_REASON_USER);
        }
        result.time[round] = MCC_GetGCTimeUs();
        result.freed[round] = MCC_GetGCFreedSize();
    }
    mutator->SetManagedContext(true);
    return nullptr;
}

void CheckTotals(bool minor, bool checkTime)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 128 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    params.gcParam.concGCThreadsSet = true;
    params.gcParam.youngGCThreads = 2;
    params.gcParam.youngGCThreadsSet = true;
    params.gcParam.oldGCThreads = 2;
    params.gcParam.oldGCThreadsSet = true;
    params.gcParam.staticGCThreads = true;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    Totals result{minor};
    auto task = RunCJTask(CollectTotals, &result);
    GC_EXPECT_TRUE(task != nullptr);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    bool valid = true;
    for (unsigned round = 1; round < 4; ++round) {
        const auto current = checkTime ? result.time[round] : result.freed[round];
        const auto previous = checkTime ? result.time[round - 1] : result.freed[round - 1];
        const auto initial = checkTime ? result.time[0] : result.freed[0];
        const bool ok = current > initial && current >= previous;
        std::fprintf(stderr, "GC_TOTALS_TARGET minor=%d metric=%s round=%u value=%llu previous=%llu initial=%llu ok=%d\n",
            minor, checkTime ? "time" : "freed", round,
            static_cast<unsigned long long>(current), static_cast<unsigned long long>(previous),
            static_cast<unsigned long long>(initial), ok);
        valid &= ok;
    }
    // Print and evaluate every target before the single fatal assertion.
    GC_EXPECT_TRUE(valid);
}
}
// ZGC zDriver.cpp:173,389 -> zServiceability.cpp:202 ->
// HotSpot memoryManager.cpp:222,252 and management.cpp:838.
GC_RUNTIME_OTHER_VM_TEST(GCTotals1322, MinorTime) { CheckTotals(true, true); }
GC_RUNTIME_OTHER_VM_TEST(GCTotals1322, MinorFreed) { CheckTotals(true, false); }
GC_RUNTIME_OTHER_VM_TEST(GCTotals1322, MajorTime) { CheckTotals(false, true); }
GC_RUNTIME_OTHER_VM_TEST(GCTotals1322, MajorFreed) { CheckTotals(false, false); }
}
