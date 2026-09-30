// Minimal runtime services for B09 tests which exercise product mark handshakes.
#ifndef MRT_B09_RUNTIME_FIXTURE_HPP
#define MRT_B09_RUNTIME_FIXTURE_HPP
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Mutator/MutatorManager.h"
#include "gc_unittest.hpp"
extern "C" int CJ_ScheduleManagerInit();
namespace MapleRuntime::GcUnit {
class B09RuntimeFixture final : public Runtime {
public:
    B09RuntimeFixture()
    {
        GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
        runtime = this;
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        manager.Init();
        concurrency.Init(ConcurrencyParam{1024, 64, 1});
    }
    ~B09RuntimeFixture() override { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam{}; }
    void SetGCThreshold(uint64_t) override {}
private:
    MutatorManager manager;
    Concurrency concurrency;
};
}
#endif
