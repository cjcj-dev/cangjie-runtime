// Minimal runtime services for B09 tests which exercise product mark handshakes.
#ifndef MRT_B09_RUNTIME_FIXTURE_HPP
#define MRT_B09_RUNTIME_FIXTURE_HPP
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Mutator/MutatorManager.h"
#include "Heap/z/zHeap.hpp"
#include "gc_unittest.hpp"
extern "C" int CJ_ScheduleManagerInit();
namespace MapleRuntime::GcUnit {
class B09RuntimeFixture final : public Runtime {
public:
    explicit B09RuntimeFixture(size_t heapUnits = 1024)
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
    ~B09RuntimeFixture() override
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
#endif
