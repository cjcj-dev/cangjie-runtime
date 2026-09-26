// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Pair with run_return_poll_pair.sh and the LLVM #12 candidate.
// The native bridge only supplies the managed TLS register and ABI alignment.
// LLVM emits the poll, site metadata and jump; the product owns all root work.
// HotSpot runtime/safepoint.cpp:818-839 protects returned oops across requests.
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/Handshake.h"
#include "Heap/z/zAddress.inline.hpp"
#include "CangjieRuntime.h"
#include <cstdio>
#include "Loader/ElfUnloadQuiescence.h"
extern "C" void ref_ret();
using namespace MapleRuntime;
class Rewrite final : public HandshakeClosure {
public:
 BaseObject *from, *to; bool seen=false;
 Rewrite(BaseObject* a, BaseObject* b):HandshakeClosure("return-pair"),from(a),to(b){}
 void do_thread(ThreadLocalData* tls) override {
  tls->mutator->VisitMutatorRoots([&](RootSlot& slot) {
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
 ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(&ref_ret));
 ThreadLocalData* tls=ThreadLocal::GetThreadLocalData();
 Mutator owner; owner.SetManagedContext(false); tls->SetMutator(&owner);
 alignas(16) char a[16]={},b[16]={};
 auto from=reinterpret_cast<BaseObject*>(a), to=reinterpret_cast<BaseObject*>(b);
 auto disarmed = Invoke(tls,from);
 if (disarmed != from) return 2;
 Rewrite change(from,to); HandshakeOperation op(&change,tls);
 Handshake::Current().add_operation(&op);
 auto result=Invoke(tls,from);
 const bool safeRegion = owner.InSaferegion();
 bool good=change.seen && result==to && !safeRegion;
 std::fprintf(stderr,"PAIR_RETURN_TARGET seen=%d result=%p expected=%p poll=%lx saferegion=%d pass=%d\n",change.seen,result,to,tls->GetPollWord(),safeRegion,good);
 tls->SetMutator(nullptr);
 return good?0:1;
}
