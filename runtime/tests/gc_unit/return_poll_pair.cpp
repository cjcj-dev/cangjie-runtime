// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Pair with run_return_poll_pair.sh and the LLVM #12 candidate.
// The native bridge only supplies the managed TLS register and ABI alignment.
// LLVM emits the poll, site metadata and jump; the product owns all root work.
// HotSpot runtime/safepoint.cpp:818-839 protects returned oops across requests.
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/Handshake.h"
#include "Mutator/MutatorManager.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/z/zAddress.inline.hpp"
#include "CangjieRuntime.h"
#include <cstdio>
#include "Loader/ElfUnloadQuiescence.h"
extern "C" void ref_ret();
using namespace MapleRuntime;
// Runtime::Current() (Common/Runtime.h:44) is reached by Mutator::InitProtectStackAddr
// through GetConcurrencyModel(), so the harness must publish the runtime singleton
// before it constructs a Mutator.  Same shape as the in-tree gc_unit harness
// (tests/gc_unit/test_cycle_ref_saferegion.cpp:57-82): a minimal Runtime that owns
// only the managers this pair actually reaches.
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
class Rewrite final : public HandshakeClosure {
public:
 BaseObject *from, *to; bool seen=false;
 Rewrite(BaseObject* a, BaseObject* b):HandshakeClosure("return-pair"),from(a),to(b){}
 void do_thread(Mutator* thread) override {
  thread->VisitMutatorRoots([&](RootSlot& slot) {
   if (to_object(safe(slot.LoadPlain()))==from) { StorePlain(slot,from_object(to)); seen=true; }
  });
 }
};
extern "C" BaseObject* Invoke(ThreadLocalData*, BaseObject*);
asm(R"(
.text
.global Invoke
.type Invoke,@function
Invoke:
 pushq %rbp
 movq %rsp,%rbp
 pushq %r15
 subq $8,%rsp
 movq %rdi,%r15
 movq %rsi,%rdi
 callq ref_ret
 addq $8,%rsp
 popq %r15
 popq %rbp
 retq
.size Invoke,.-Invoke
)");
int main() {
 // The LLVM cangjie pipeline emits stack-growth columns in compressed maps.
 CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_ON;
 MutatorManager manager; PairConcurrencyModel model; PairRuntime instance(manager, model);
 ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(&ref_ret));
 ThreadLocalData* tls=ThreadLocal::GetThreadLocalData();
 // add_operation arms the poll words of the registered mark-flush threads
 // (Handshake.cpp:216-219 -> MutatorManager::ForEachMarkFlushTls), which is the
 // producer of the tri-state word the LLVM return poll tests.  Same registration
 // the product performs in MutatorManager::BindMutator (MutatorManager.cpp:149-155)
 // and the in-tree pair harness (tests/gc_unit/test_remap_young_roots.cpp:34-36).
 manager.RegisterMarkFlushThread(tls);
 Mutator owner; owner.SetManagedContext(false); tls->SetMutator(&owner);
 alignas(16) char a[16]={},b[16]={};
 auto from=reinterpret_cast<BaseObject*>(a), to=reinterpret_cast<BaseObject*>(b);
 auto disarmed = Invoke(tls,from);
 if (disarmed != from) return 2;
 Rewrite change(from,to); HandshakeOperation op(&change,&owner);
 Handshake::Current().add_operation(&op);
 auto result=Invoke(tls,from);
 const bool safeRegion = owner.InSaferegion();
 bool good=change.seen && result==to && !safeRegion;
 std::fprintf(stderr,"PAIR_RETURN_TARGET seen=%d result=%p expected=%p poll=%lx saferegion=%d pass=%d\n",change.seen,result,to,tls->GetPollWord(),safeRegion,good);
 tls->SetMutator(nullptr);
 return good?0:1;
}
