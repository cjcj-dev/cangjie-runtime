// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Real LLVM frames -> product safepoint entry -> observed watermark frontier.
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/Handshake.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "CangjieRuntime.h"
#include "Cangjie.h"
#include "Loader/ElfUnloadQuiescence.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
using namespace MapleRuntime;
extern "C" void native_chain(int);
extern "C" uint32_t unwindPCForC2NStub;
extern "C" uintptr_t native_anchor;
uintptr_t native_anchor;
extern "C" void invoke_native(ThreadLocalData*, int);
asm(R"(
.text
.global invoke_native
.type invoke_native,@function
invoke_native:
 pushq %rbp
 movq %rsp,%rbp
 pushq %r15
 subq $8,%rsp
 movq %rbp,native_anchor(%rip)
 movq %rdi,%r15
 movq %rsi,%rdi
 callq native_chain
 addq $8,%rsp
 popq %r15
 popq %rbp
 retq
.size invoke_native,.-invoke_native
)");
static uintptr_t initial, expected;
static bool passed, armed, contextSaved;
static int mode;
static uint32_t savedState;
class ObserveHandshake final: public HandshakeClosure {
public:
 ObserveHandshake():HandshakeClosure("native-frame-pair") {}
 void do_thread(Mutator* owner) override { initial=owner->GetStackWatermark().last_processed_raw(); }
};
static ObserveHandshake closure;
static HandshakeOperation* operation;
extern "C" void native_arm()
{
 auto& owner=*Mutator::GetMutator();
 owner.GetUnwindContext().anchorFA=reinterpret_cast<uint32_t*>(native_anchor);
 auto* stub=reinterpret_cast<uintptr_t*>(owner.GetUnwindContext().frameInfo.mFrame.GetFA());
 contextSaved=stub != nullptr && owner.GetUnwindContext().frameInfo.mFrame.GetIP()==&unwindPCForC2NStub;
 // A missing producer result reaches the target assertion after returning;
 // it must not turn into an earlier invalid frame access or assertion.
 if (!contextSaved) { return; }
 auto* fp=reinterpret_cast<uintptr_t*>(stub[0]);
 for (unsigned i=0;i<2;++i) fp=reinterpret_cast<uintptr_t*>(fp[0]);
 // Initial processing covers three barriers; before_unwind advances the
 // frontier to protect the exposed caller. Use the actual compiler frames.
 expected=reinterpret_cast<uintptr_t>(fp+2);
 savedState=owner.GetStackWatermark().PackedState();
 ZGlobalsPointers::flip_old_relocate_start();
 if (armed) {
  operation=new HandshakeOperation(&closure,&owner);
  Handshake::Current().add_operation(operation);
 } else if (mode==2) {
  StackWatermarkSet::start_processing(owner);
  initial=owner.GetStackWatermark().last_processed_raw();
  savedState=owner.GetStackWatermark().PackedState();
  UpdatePollValues(ThreadLocal::GetThreadLocalData());
 }
}
extern "C" void native_observe()
{
 auto& owner=*Mutator::GetMutator();
 auto& watermark=owner.GetStackWatermark();
 auto frontier=watermark.last_processed_raw();
 passed=contextSaved && (armed ? initial!=0 && frontier>initial && frontier==expected && !owner.InSaferegion()
              : mode==2 ? initial!=0 && frontier==initial && watermark.PackedState()==savedState && !watermark.IsDone() && !owner.InSaferegion()
              : frontier==0 && watermark.PackedState()==savedState && !watermark.processing_started() && !owner.InSaferegion());
 std::fprintf(stderr,"NATIVE_FRAME_PAIR_TARGET mode=%d saved=%d initial=%p frontier=%p expected=%p lazy=%d active=%d pass=%d executed=1\n",mode,contextSaved,(void*)initial,(void*)frontier,(void*)expected,!watermark.processing_started(),!owner.InSaferegion(),passed);
 // End the experiment before return-poll processing changes its result.
 watermark.Reset();
 owner.SetManagedContext(false);
 std::fflush(nullptr);
 std::_Exit(passed?0:1);
}
int main(int argc,char** argv)
{
 mode=argc>1?std::atoi(argv[1]):0;
 armed=mode==1;
 const int depth=argc>2?std::atoi(argv[2]):9;
 CangjieRuntime::stackGrowConfig=StackGrowConfig::STACK_GROW_ON;
 RuntimeParam param {}; param.coParam.processorNum=1; param.heapParam.heapSize=32*1024;
 if (InitCJRuntime(&param)!=E_OK) return 4;
 auto& manager=MutatorManager::Instance();
 ZGlobalsPointers::initialize();
 ElfUnloadQuiescence::LinkImage(reinterpret_cast<Uptr>(&native_chain));
 auto* tls=ThreadLocal::GetThreadLocalData();
 manager.RegisterMarkFlushThread(tls);
 Mutator owner; owner.SetManagedContext(false); tls->SetMutator(&owner);
 MRT_LeaveSaferegion(); owner.SetManagedContext(true);
 owner.GetGCData().InstallMasks(ThreadGCData::PublishedMasks());
 invoke_native(tls,depth);
 return 3;
}
