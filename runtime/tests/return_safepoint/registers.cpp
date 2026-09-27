// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Directly calls the linked product stub, as required by runtime#1178.
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include <cstdio>
#include <cstdint>
using namespace MapleRuntime;
class StubConcurrency final : public ConcurrencyModel {
public:
 void VisitGCRoots(RootVisitor*) override {}
 size_t GetReservedStackSize() const override { return 0; }
 bool GetStackGuardCheckFlag() const override { return false; }
};
class StubRuntime final : public Runtime {
public:
 StubRuntime(MutatorManager& manager, ConcurrencyModel& model) {
  mutatorManager = &manager; concurrencyModel = &model; runtime = this;
 }
 ~StubRuntime() override { runtime = nullptr; }
 RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
 void SetGCThreshold(uint64_t) override {}
};
struct Snapshot {
 uint64_t before[20], after[20];
 uint64_t vectorsBefore[64], vectorsAfter[64];
};
extern "C" void CheckReturnStub(ThreadLocalData*, Snapshot*);
int main() {
 MutatorManager manager; StubConcurrency model; StubRuntime runtime(manager, model);
 auto* tls = ThreadLocal::GetThreadLocalData();
 Mutator mutator; mutator.SetManagedContext(false); tls->SetMutator(&mutator);
 Snapshot snapshot {};
 // The bridge passes zero metadata PCs: these are primitive register values,
 // not reference returns. No synthetic root map or runtime replacement is used.
 CheckReturnStub(tls, &snapshot);
#if defined(__aarch64__)
 const char* names[] = {"x0","x1","x2","x3","x4","x5","x6","x7","x8",
  "x19","x20","x21","x22","x23","x24","x25","x26","x27","x28","x29"};
 constexpr unsigned vectors = 32;
#else
 const char* names[] = {"rax","rdx","rbx","rbp","rsi","rdi","r12","r13","r14","r15"};
 constexpr unsigned vectors = 16;
#endif
 unsigned failures = 0, checked = 0;
 for (unsigned i=0; i<sizeof(names)/sizeof(names[0]); ++i) {
  bool good = snapshot.before[i] == snapshot.after[i]; ++checked; failures += !good;
  std::printf("REGISTER_ASSERT %s before=%llx after=%llx %s\n", names[i],
   (unsigned long long)snapshot.before[i], (unsigned long long)snapshot.after[i], good?"PASS":"FAIL");
 }
 for (unsigned i=0; i<vectors; ++i) {
  bool good = snapshot.vectorsBefore[2*i] == snapshot.vectorsAfter[2*i] &&
   snapshot.vectorsBefore[2*i+1] == snapshot.vectorsAfter[2*i+1];
  ++checked; failures += !good;
  std::printf("REGISTER_ASSERT vector%u %s\n", i, good?"PASS":"FAIL");
 }
 std::printf("REGISTER_RESULT checked=%u failures=%u\n",checked,failures);
 tls->SetMutator(nullptr);
 return failures?1:0;
}
