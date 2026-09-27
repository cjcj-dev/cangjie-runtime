// Win64 ordinary safepoint ABI: invoke the exported product stub, then read
// its restored SIMD values. No replacement runtime entry or product hook.
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include <cstdio>
#include <cstdint>
using namespace MapleRuntime;
class PairConcurrencyModel final : public ConcurrencyModel {
public:
 void VisitGCRoots(RootVisitor*) override {}
 size_t GetReservedStackSize() const override { return 0; }
 bool GetStackGuardCheckFlag() const override { return false; }
};
class PairRuntime final : public Runtime {
public:
 PairRuntime(MutatorManager& manager, ConcurrencyModel& model)
 {
  mutatorManager = &manager; concurrencyModel = &model; runtime = this;
 }
 ~PairRuntime() override { runtime = nullptr; }
 RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
 void SetGCThreshold(uint64_t) override {}
};

extern "C" void InvokeOrdinary(ThreadLocalData*, const uint64_t*, uint64_t*);
int main()
{
    MutatorManager manager;
    PairConcurrencyModel model;
    PairRuntime instance(manager, model);
    Mutator owner;
    owner.SetManagedContext(false);
    auto* tls = ThreadLocal::GetThreadLocalData();
    tls->SetMutator(&owner);
    // A request may be disarmed before a thread reaches the stub. Exercise
    // that valid path without requiring a synthetic managed stack map.
    tls->SetPollWord(ThreadLocalData::DisarmedPollWord);
    const uint64_t expected[4] = {0x123456789abcdef0ULL, 0xfedcba9876543210ULL,
                                  0x55aa33cc77ee1199ULL, 0x1122446688aacceeULL};
    uint64_t actual[4] = {};
    InvokeOrdinary(tls, expected, actual);
    int failures = 0;
    for (int reg = 0; reg != 2; ++reg) {
        bool ok = actual[2*reg] == expected[2*reg] && actual[2*reg+1] == expected[2*reg+1];
        std::printf("SIMD_TARGET xmm%d lo=%016llx hi=%016llx pass=%d\n",
                    14+reg, (unsigned long long)actual[2*reg],
                    (unsigned long long)actual[2*reg+1], ok);
        failures += !ok;
    }
    std::fflush(stdout);
    tls->SetMutator(nullptr);
    return failures ? 1 : 0;
}
