#pragma once
#include "gc_unittest.hpp"
#include "Common/Runtime.h"
#include "Heap/z/zHeap.hpp"
#include "Concurrency/Concurrency.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/VMOperation.h"
extern "C" int CJ_ScheduleManagerInit();
namespace MapleRuntime::GcUnit {
// Container only. The operation itself is the product VMThread::execute path,
// and the phase call below is the product ZGeneration pause entry.
class VMThreadContainerRuntime final : public Runtime {
public:
    explicit VMThreadContainerRuntime(size_t heapUnits)
    {
        CreateStandaloneHeap(heapUnits);
        GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
        runtime = this;
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        manager.Init();
        concurrency.Init(ConcurrencyParam{1024, 64, 1});
        VMThread::create();
    }
    ~VMThreadContainerRuntime() override
    {
        Heap::GetHeap().StopGCWork();
        VMThread::wait_for_vm_thread_exit();
        runtime = nullptr;
    }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam{}; }
    void SetGCThreshold(uint64_t) override {}

private:
    MutatorManager manager;
    Concurrency concurrency;
};

}
