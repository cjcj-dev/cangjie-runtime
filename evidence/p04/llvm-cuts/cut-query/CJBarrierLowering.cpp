//===-- GCBarrierLowering.cpp - Cangjie Barrier Lowering ------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file implements the lowering for the cangjie barrier mechanism.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/CJIntrinsics.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/IR/CJStructTypeGCInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCContext.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <unordered_map>

using namespace llvm;
using namespace cangjie;

#define DEBUG_TYPE "cj-barrier-lowering"

static cl::opt<bool> EnableTaggedPointer("enable-tagged-pointer",
                                         cl::init(true), cl::Hidden);
static cl::opt<bool> EnableGCPhase("enable-gc-phase", cl::init(true),
                                   cl::Hidden);
static cl::opt<bool> EnableGCFastPath("enable-gc-fast-path", cl::init(true),
                                      cl::Hidden);

namespace llvm {
extern cl::opt<bool> CangjieJIT;
extern cl::opt<bool> DisableGCSupport;
extern cl::opt<bool> EnableSafepointOnly;
extern cl::opt<bool> EnableSafepointOutline;
} // namespace llvm

namespace {
const static StringRef NewObjFastStr = "CJ_MCC_NewObjectFast";
const static StringRef NewObjFinalizerFastStr = "CJ_MCC_NewFinalizerFast";
constexpr StringRef SafepointStub = "CJ_Safepoint_Stub";
// Runtime P04 publishes every ZGC reservation (ZMaxVirtualReservations=100).
// This table is the P01 slot-domain ABI; query only the live entries.
constexpr unsigned kCjHeapRangeCap = 100;

static Value *emitReservedHeapSlot(IRBuilder<> &Builder, Module *M, Value *PlaceI,
                                   const Twine &Name) {
  LLVMContext &C = M->getContext();
  Type *I64 = Type::getInt64Ty(C);
  ArrayType *ArrTy = ArrayType::get(I64, kCjHeapRangeCap);
  Value *Count = Builder.CreateLoad(
      I64, M->getOrInsertGlobal("g_cjHeapRangeCount", I64), Name + ".n");
  Constant *Starts = M->getOrInsertGlobal("g_cjHeapRangeStart", ArrTy);
  Constant *Ends = M->getOrInsertGlobal("g_cjHeapRangeEnd", ArrTy);

  // Keep the decision at this helper's original read/store call sites. The
  // runtime count controls a bounded loop, rather than unrolling empty slots.
  BasicBlock *Entry = Builder.GetInsertBlock();
  Instruction *ResumeAt = &*Builder.GetInsertPoint();
  Function *F = Entry->getParent();
  BasicBlock *Done = Entry->splitBasicBlock(ResumeAt, Name + ".done");
  BasicBlock *Check = BasicBlock::Create(C, Name + ".check", F, Done);
  BasicBlock *Loop = BasicBlock::Create(C, Name + ".loop", F, Done);
  BasicBlock *Next = BasicBlock::Create(C, Name + ".next", F, Done);
  BasicBlock *Invalid = BasicBlock::Create(C, Name + ".invalid", F, Done);
  Entry->getTerminator()->eraseFromParent();
  Builder.SetInsertPoint(Entry);
  Value *Valid = Builder.CreateICmpULE(Count, Builder.getInt64(kCjHeapRangeCap),
                                      Name + ".count.valid");
  Builder.CreateCondBr(Valid, Check, Invalid);

  Builder.SetInsertPoint(Invalid);
  Builder.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::trap));
  Builder.CreateUnreachable();

  Builder.SetInsertPoint(Check);
  Builder.CreateCondBr(Builder.CreateICmpNE(Count, Builder.getInt64(0)), Loop, Done);

  Builder.SetInsertPoint(Loop);
  PHINode *Index = Builder.CreatePHI(I64, 2, Name + ".index");
  Index->addIncoming(Builder.getInt64(0), Check);
  Value *SPtr = Builder.CreateInBoundsGEP(
      ArrTy, Starts, {Builder.getInt64(0), Index}, Name + ".sp");
  Value *EPtr = Builder.CreateInBoundsGEP(
      ArrTy, Ends, {Builder.getInt64(0), Index}, Name + ".ep");
  Value *Start = Builder.CreateLoad(I64, SPtr, Name + ".start");
  Value *End = Builder.CreateLoad(I64, EPtr, Name + ".end");
  Value *Hit = Builder.CreateAnd(Builder.CreateICmpUGE(PlaceI, Start),
                                  Builder.CreateICmpULE(PlaceI, End), Name + ".hit");
  Builder.CreateCondBr(Hit, Done, Next);

  Builder.SetInsertPoint(Next);
  Value *NextIndex = Builder.CreateAdd(Index, Builder.getInt64(1), Name + ".next.index");
  Index->addIncoming(NextIndex, Next);
  Builder.CreateCondBr(Builder.CreateICmpULT(NextIndex, Count), Loop, Done);

  Builder.SetInsertPoint(ResumeAt);
  PHINode *In = Builder.CreatePHI(Builder.getInt1Ty(), 3, Name + ".result");
  In->addIncoming(Builder.getFalse(), Check);
  In->addIncoming(Builder.getTrue(), Loop);
  In->addIncoming(Builder.getFalse(), Next);
  return In;
}
template <typename KeyT, typename ValT>
using StdMap = std::unordered_map<KeyT, ValT>;
const static StdMap<unsigned, StringRef> IntrinsicMap{
    {Intrinsic::cj_gcwrite_ref, "CJ_MCC_WriteRefField"},
    {Intrinsic::cj_gcwrite_struct, "CJ_MCC_WriteStructField"},
    {Intrinsic::cj_gcwrite_static_ref, "CJ_MCC_WriteStaticRef"},
    {Intrinsic::cj_gcwrite_static_struct, "CJ_MCC_WriteStaticStruct"},
    {Intrinsic::cj_gcread_ref, "CJ_MCC_ReadRefField"},
    {Intrinsic::cj_gcread_weakref, "CJ_MCC_ReadWeakRef"},
    {Intrinsic::cj_gcread_struct, "CJ_MCC_ReadStructField"},
    {Intrinsic::cj_gcread_static_ref, "CJ_MCC_ReadStaticRef"},
    {Intrinsic::cj_gcread_static_struct, "CJ_MCC_ReadStaticStruct"},
    {Intrinsic::cj_copy_struct_field, "CJ_MCC_CopyStructField"},
    {Intrinsic::cj_array_copy_ref, "CJ_MCC_ArrayCopyRef"},
    {Intrinsic::cj_array_copy_struct, "CJ_MCC_ArrayCopyStruct"},
    {Intrinsic::cj_atomic_store, "CJ_MCC_AtomicWriteReference"},
    {Intrinsic::cj_atomic_load, "CJ_MCC_AtomicReadReference"},
    {Intrinsic::cj_atomic_swap, "CJ_MCC_AtomicSwapReference"},
    {Intrinsic::cj_atomic_compare_swap, "CJ_MCC_AtomicCompareAndSwapReference"},
    {Intrinsic::cj_assign_generic, "CJ_MCC_AssignGeneric"},
    {Intrinsic::cj_gcwrite_generic, "CJ_MCC_WriteGeneric"},
    {Intrinsic::cj_gcread_generic, "CJ_MCC_ReadGeneric"},
    {Intrinsic::cj_array_copy_generic, "CJ_MCC_ArrayCopyGeneric"},
    {Intrinsic::cj_gcwrite_generic_payload, "CJ_MCC_WriteGenericPayload"},
    {Intrinsic::cj_gcread_generic_payload, "CJ_MCC_ReadGenericPayload"}};

struct BBInfo {
  // BB last used barrier after safepoint
  CallBase *LastBarrier;

  bool HasCalls; // has GC Calls

  //  there are barriers before first safepoint
  bool BeforeCall;
  bool Visited;
  Instruction *OutCheck;
  Instruction *InCheck;

  // 8: default preBBs size
  SmallVector<BasicBlock *, 8> PreBBs;

  BBInfo() : HasCalls(false), BeforeCall(false), Visited(false) {
    LastBarrier = nullptr;
    OutCheck = nullptr;
    InCheck = nullptr;
  }
};

static bool mayBeSafepoint(Instruction *Inst) {
  if (auto CI = dyn_cast<CallBase>(Inst)) {
    if (CI->getIntrinsicID() == Intrinsic::cj_gc_statepoint) {
      return true;
    }
  }
  return false;
}

/// Declarations for Cangjie barrier functions.
class BarrierMaker {
public:
  BarrierMaker(Module &M, CJStructTypeGCInfo &GCInfo)
      : M(M), C(M.getContext()), GCInfo(GCInfo) {
    GCPtr = Type::getInt8PtrTy(C, 1);
    I8Ptr = Type::getInt8PtrTy(C);
    I64 = Type::getInt64Ty(C);
    I32 = Type::getInt32Ty(C);
  }

  ~BarrierMaker() = default;

  void replaceInstWithBarrier(IntrinsicInst *II) {
    if (II->isTailCall()) {
      II->setTailCall(false);
    }

    Function *Callee = getOrInsertRuntimeFunc(II);
    switch (II->getIntrinsicID()) {
    case Intrinsic::cj_gcwrite_struct: {
      Value *GCTib = getOrInsertGCTib(II);
      Value *Param[6] = {II->getArgOperand(GCWriteStruct::BaseObj),
                         II->getArgOperand(GCWriteStruct::Dst),
                         II->getArgOperand(GCWriteStruct::Size),
                         II->getArgOperand(GCWriteStruct::Src),
                         II->getArgOperand(GCWriteStruct::Size), GCTib};
      // gcwrite.agg src has two case:
      // i8* on the stack and i8 addrspace(1)* on the heap.
      // The runtime function has only one signature. Here, the src of i8*
      // is converted to i8 addrspace(1)*.
      if (!isGCPointerType(II->getArgOperand(GCWriteStruct::Src)->getType())) {
        Param[3] = new AddrSpaceCastInst(II->getArgOperand(GCWriteStruct::Src),
                                         Type::getInt8PtrTy(C, 1), "", II);
      }
      // Replace the original intrinsic with the runtime function.
      replaceCallInst(Callee, Param, II);
      break;
    }
    case Intrinsic::cj_gcread_struct: {
      Value *GCTib = getOrInsertGCTib(II);
      Value *Param[5] = {II->getArgOperand(GCReadStruct::Dst),
                         II->getArgOperand(GCReadStruct::BaseObj),
                         II->getArgOperand(GCReadStruct::Src),
                         II->getArgOperand(GCReadStruct::Size), GCTib};
      // Replace the original intrinsic with the runtime function.
      replaceCallInst(Callee, Param, II);
      break;
    }
    case Intrinsic::cj_gcread_static_struct:
    case Intrinsic::cj_gcwrite_static_struct: {
      Value *GCTib = getOrInsertGCTib(II);
      Value *Param[5] = {II->getArgOperand(GCWriteStaticStruct::Dst),
                         II->getArgOperand(GCWriteStaticStruct::Size),
                         II->getArgOperand(GCWriteStaticStruct::Src),
                         II->getArgOperand(GCWriteStaticStruct::Size), GCTib};
      // Replace the original intrinsic with the runtime function.
      replaceCallInst(Callee, Param, II);
      break;
    }
    case Intrinsic::cj_array_copy_ref:
    case Intrinsic::cj_array_copy_struct:
    case Intrinsic::cj_array_copy_generic: {
      Value *Param[6] = {II->getArgOperand(ArrayCopy::DstObj),
                         II->getArgOperand(ArrayCopy::DstPtr),
                         II->getArgOperand(ArrayCopy::Size),
                         II->getArgOperand(ArrayCopy::SrcObj),
                         II->getArgOperand(ArrayCopy::SrcPtr),
                         II->getArgOperand(ArrayCopy::Size)};
      // Replace the original intrinsic with the runtime function.
      replaceCallInst(Callee, Param, II);
      break;
    }
    default:
      II->setCalledFunction(Callee);
      break;
    }
  }

  void replaceCallInst(Function *Callee, ArrayRef<Value *> Args,
                       IntrinsicInst *II) const {
    IRBuilder<> Builder(II);
    CallInst *NewCI = Builder.CreateCall(Callee, Args);
    II->replaceAllUsesWith(NewCI);
    II->eraseFromParent();
  }

private:
  Module &M;
  LLVMContext &C;
  CJStructTypeGCInfo &GCInfo;
  DenseMap<StringRef, Function *> RTFuncMap;
  Type *GCPtr;
  Type *I8Ptr;
  Type *I64;
  Type *I32;

  StringRef getRuntimeFuncName(IntrinsicInst *II) {
    Intrinsic::ID IID = II->getIntrinsicID();
    auto Itr = IntrinsicMap.find(IID);
    assert(Itr != IntrinsicMap.end() && "Runtime Intrinsic don`t exist.");
    return Itr->second;
  }

  Function *getOrInsertRuntimeFunc(IntrinsicInst *II) {
    StringRef Callee = getRuntimeFuncName(II);
    assert(Callee != "" && "Callee don`t exist.");
    auto Itr = RTFuncMap.find(Callee);
    if (Itr != RTFuncMap.end())
      return Itr->second;
    // add dstLen to runtime API
    FunctionType *FuncType = nullptr;
    const Triple TT(II->getModule()->getTargetTriple());
    auto isARM = TT.isARM();
    switch (II->getIntrinsicID()) {
    case Intrinsic::cj_gcwrite_struct: {
      Type *ParamType[6] = {GCPtr, GCPtr, I64, GCPtr, I64, I8Ptr};
      if (isARM)
        ParamType[2] = ParamType[4] = I32;
      FuncType = FunctionType::get(Type::getVoidTy(C), ParamType, false);
      break;
    }
    case Intrinsic::cj_gcread_struct: {
      Type *ParamType[5] = {I8Ptr, GCPtr, GCPtr, I64, I8Ptr};
      if (isARM)
        ParamType[3] = I32;
      FuncType = FunctionType::get(Type::getVoidTy(C), ParamType, false);
      break;
    }
    case Intrinsic::cj_gcread_static_struct:
    case Intrinsic::cj_gcwrite_static_struct: {
      Type *ParamType[5] = {I8Ptr, I64, I8Ptr, I64, I8Ptr};
      if (isARM)
        ParamType[1] = ParamType[3] = I32;
      FuncType = FunctionType::get(Type::getVoidTy(C), ParamType, false);
      break;
    }
    case Intrinsic::cj_array_copy_ref:
    case Intrinsic::cj_array_copy_struct:
    case Intrinsic::cj_array_copy_generic: {
      Type *ParamType[6] = {GCPtr, GCPtr, I64, GCPtr, GCPtr, I64};
      if (isARM)
        ParamType[2] = ParamType[5] = I32;
      FuncType = FunctionType::get(Type::getVoidTy(C), ParamType, false);
      break;
    }
    default:
      FuncType = II->getFunctionType();
      break;
    }
    Function *Func =
        cast<Function>(M.getOrInsertFunction(Callee, FuncType).getCallee());
    RTFuncMap.insert({Callee, Func});
    return Func;
  }

  Constant *getOrInsertGCTib(IntrinsicInst *II) const {
    const MDNode *Metadata = II->getMetadata("AggType");
    assert(Metadata && "Missing AggType Metadata.");
    assert(Metadata->getNumOperands() == 1 &&
           "AggType meta's size should be equal to 1!");

    auto STName =
        dyn_cast<MDString>(Metadata->getOperand(0).get())->getString();
    auto *ST = StructType::getTypeByName(C, STName);
    assert(ST && "AggType doesn't exsit.");

    Type *DstType = Type::getInt8PtrTy(C);
    std::string BitMapName = STName.str();
    Constant *BitMapGV = M.getGlobalVariable(BitMapName + ".bitmap");
    if (BitMapGV == nullptr) {
      auto &Info = GCInfo.getOrInsertTypeGCInfo(ST);
      BitMapGV =
          GCInfo.getOrInsertBitMap(Info.BMInfo.BMStr, DstType, BitMapName);
    } else {
      BitMapGV = ConstantExpr::getBitCast(BitMapGV, DstType);
    }
    assert(BitMapGV && "BitMapGV get or insert fail");
    return BitMapGV;
  }
};

// Insert gc-phase check for write barriers.
class GCPhaseCheck {
public:
  explicit GCPhaseCheck(Function &F) : C(F.getContext()) {
    GCStateCheckFunc = F.getParent()->getFunction("GetGCPhase");
    assert(GCStateCheckFunc && "Has no GetGCPhase");
  };
  ~GCPhaseCheck() = default;

  void initBBInfo(Function &F) {
    BBInfosMap.clear();
    Barriers.clear();
    BarrierCheckMap.clear();
    for (auto &BB : F) {
      BBInfosMap[&BB] = BBInfo();
    }
  }

  void setLastBarrier(CallBase *CI) {
    BasicBlock *BB = CI->getParent();
    auto &Info = BBInfosMap[BB];
    if (Info.LastBarrier == nullptr) {
      Info.LastBarrier = CI;
      if (Info.HasCalls) {
        createCheck(CI);
      }
    }
    if (!Info.HasCalls) {
      Info.BeforeCall = true;
    }
  }

  void setHasCall(BasicBlock *BB) {
    auto &Info = BBInfosMap[BB];
    Info.HasCalls = true;
    Info.LastBarrier = nullptr;
  }

  bool isNullPointer(Value *V) {
    if (isa<Constant>(V)) {
      return V->getValueID() == Value::ConstantPointerNullVal;
    }
    return false;
  }

  // A null base object is not a storage-class proof.  Raw lowering is valid
  // only when the destination itself is rooted in an alloca or AS0; an
  // unknown AS1 destination may still be a heap reference slot and must stay
  // on the MCC path so the runtime publishes a coloured word.
  bool hasProvenNonHeapDestination(CallBase *CI) {
    Value *Dst = nullptr;
    switch (CI->getIntrinsicID()) {
    case Intrinsic::cj_gcwrite_ref:
      Dst = getPointerArg(CI);
      break;
    case Intrinsic::cj_gcwrite_struct:
      Dst = getDest(CI);
      break;
    default:
      return false;
    }

    auto *DstTy = dyn_cast<PointerType>(Dst->getType());
    if (DstTy && DstTy->getAddressSpace() == 0)
      return true;

    Value *Base = findMemoryBasePointer(Dst);
    if (isa<AllocaInst>(Base))
      return true;
    auto *BaseTy = dyn_cast<PointerType>(Base->getType());
    return BaseTy && BaseTy->getAddressSpace() == 0;
  }

  bool fastBarrier(unsigned IID, CallBase *CI) {
    switch (IID) {
    // Stack/AS0 writes have no HeapSlot colour contract.  Null alone proves
    // nothing: keep an unproven AS1 destination on MCC.
    case Intrinsic::cj_gcwrite_ref:
    case Intrinsic::cj_gcwrite_struct: {
      Value *BaseObj = getBaseObj(CI);
      if (isNullPointer(BaseObj) && hasProvenNonHeapDestination(CI)) {
        IRBuilder<> Builder(CI);
        createStoreOrMems(CI, Builder);
        CI->eraseFromParent();
        return true;
      }
      break;
    }
    // non-constant ordering.
    case Intrinsic::cj_atomic_store:
    case Intrinsic::cj_atomic_swap:
      if (!isa<Constant>(getAtomicOrder(CI))) {
        return false;
      }
      break;
    case Intrinsic::cj_atomic_compare_swap:
      if (!isa<Constant>(CI->getArgOperand(AtomicCompareSwap::SuccOrder)) ||
          !isa<Constant>(CI->getArgOperand(AtomicCompareSwap::FailOrder))) {
        return false;
      }
      break;
    default:
      break;
    }

    // A generational collector needs the runtime path for these even while no
    // tracing cycle is active, so old-to-young writes enter its remembered set.
    // Static roots also require store-good colouring in every phase, including
    // initialization (ZBarrierSet::AccessBarrier::oop_store_not_in_heap).
    // Never lower them to a phase-guarded plain store.
    if (IID == Intrinsic::cj_gcwrite_ref ||
        IID == Intrinsic::cj_gcwrite_static_ref ||
        IID == Intrinsic::cj_gcwrite_struct ||
        IID == Intrinsic::cj_gcwrite_static_struct ||
        IID == Intrinsic::cj_array_copy_ref ||
        IID == Intrinsic::cj_array_copy_struct ||
        IID == Intrinsic::cj_atomic_store) {
      return false;
    }

    setLastBarrier(CI);
    Barriers.push_back(CI);
    return true;
  }

  void updateBBOut(BasicBlock *BB) {
    BBInfo &Info = BBInfosMap[BB];
    if (Info.HasCalls && Info.LastBarrier != nullptr) {
      Info.OutCheck = BarrierCheckMap[Info.LastBarrier];
      assert(Info.OutCheck && "out check don't generate.");
    }
  }

  void finishAll() {
    updateBBInfo();
    replaceBarrier();
  }

  void createGCCheck(BasicBlock *InsertBB, BasicBlock *True,
                     BasicBlock *False) {
    IRBuilder<> IRB(InsertBB);
    CallInst *GCState = IRB.CreateCall(GCStateCheckFunc);
    GCState->setCallingConv(CallingConv::CangjieGC);
    auto CheckResult = IRB.CreateICmpSLE(
        GCState, Constant::getIntegerValue(GCState->getType(), APInt(32, 8)));
    IRB.CreateCondBr(CheckResult, True, False);
  }

private:
  LLVMContext &C;
  Function *GCStateCheckFunc = nullptr;
  SmallVector<CallBase *, 8> Barriers;
  SmallMapVector<BasicBlock *, BBInfo, 8> BBInfosMap;
  SmallMapVector<Instruction *, Instruction *, 8> BarrierCheckMap;

  Instruction *createCheck(CallBase *CI) {
    Instruction *CheckResult = BarrierCheckMap[CI];
    if (CheckResult)
      return CheckResult;
    IRBuilder<> Builder(CI);
    // direct load from thread local if possible
    CallInst *GCState = Builder.CreateCall(GCStateCheckFunc);
    GCState->setCallingConv(CallingConv::CangjieGC);
    // 8: related and equal to enum GCPhase::kGCPhaseInit.
    CheckResult = dyn_cast<Instruction>(Builder.CreateICmpSLE(
        GCState, Constant::getIntegerValue(GCState->getType(), APInt(32, 8))));
    BarrierCheckMap[CI] = CheckResult;
    return CheckResult;
  }

  Instruction *createPhi(BasicBlock *BB,
                         SmallVector<Instruction *, 8> &PhiValues) {
    auto &Info = BBInfosMap[BB];
    unsigned IncomingSize = PhiValues.size();
    Type *CheckType = PhiValues[0]->getType();
    IRBuilder<> Builder(BB->getFirstNonPHI());
    PHINode *Phi = Builder.CreatePHI(CheckType, IncomingSize);
    for (unsigned i = 0; i < PhiValues.size(); i++) {
      Phi->addIncoming(PhiValues[i], Info.PreBBs[i]);
    }
    return Phi;
  }

  void updateBBInfo() {
    for (auto &Info : BBInfosMap) {
      BasicBlock *CurBB = Info.first;
      for (BasicBlock *Succ : successors(CurBB)) {
        auto &BBInfo = BBInfosMap[Succ];
        BBInfo.PreBBs.push_back(CurBB);
      }
    }
  }

  void resetLastCheckResult(Instruction *&CR, BasicBlock *CurBB, unsigned i) {
    for (BasicBlock::iterator BBInst = CurBB->begin();;) {
      if (Barriers[i] == &*BBInst)
        break;
      if (mayBeSafepoint(&*BBInst)) {
        CR = nullptr;
        break;
      }
      BBInst++;
    }
  }

  Instruction *createFastInstr(BasicBlock *TrueBranch, CallBase *CI,
                               const DebugLoc &CurDbg) {
    IRBuilder<> Builder(TrueBranch->getTerminator());
    Instruction *NewInst = createStoreOrMems(CI, Builder);
    if (NewInst != nullptr) {
      NewInst->setDebugLoc(CurDbg);
    }
    return NewInst;
  }

  void handleSuccPhi(CallBase *CI, BasicBlock *FalseBranch, Instruction *New,
                     BasicBlock *TrueBranch, BasicBlock *Succ) {
    Intrinsic::ID IID = CI->getIntrinsicID();
    if (IID != Intrinsic::cj_atomic_swap &&
        IID != Intrinsic::cj_atomic_compare_swap) {
      return;
    }
    Value *Result = nullptr;
    IRBuilder<> BuilderExtract(TrueBranch->getTerminator());
    if (IID == Intrinsic::cj_atomic_compare_swap) {
      Result = BuilderExtract.CreateExtractValue(New, 1, "swapResult");
    } else {
      Result = BuilderExtract.CreateIntToPtr(
          New, Type::getInt8Ty(CI->getContext())->getPointerTo(1));
    }
    IRBuilder<> Builder(Succ->getFirstNonPHI());
    PHINode *Phi = Builder.CreatePHI(CI->getType(), 2);
    CI->replaceAllUsesWith(Phi);
    Phi->addIncoming(CI, FalseBranch);
    Phi->addIncoming(Result, TrueBranch);
  }
  // do the following conversion:
  //   call void @llvm.cj.gcwrite
  // =====>
  //   br i1 CondVal, label %gcNoRunning, label %gcRunning
  // gcRunning:
  //   call void @llvm.cj.gcwrite
  //   br label %storeFinish
  // gcNoRunning:
  //   store xxx
  //   br label %storeFinish
  // storeFinish:
  //   ...
  BasicBlock *SplitFastPathAndSlowPath(CallBase *CurInst, BasicBlock *SplitBB,
                                       Value *CondVal) {
    const DebugLoc &CurDbg = CurInst->getDebugLoc();
    BasicBlock *FalseBranch = SplitBB->splitBasicBlock(CurInst, "gcRunning");
    BasicBlock *Succ =
        FalseBranch->splitBasicBlock(CurInst->getNextNode(), "storeFinish");
    BasicBlock *TrueBranch =
        BasicBlock::Create(C, "gcNoRunning", SplitBB->getParent(), Succ);
    BranchInst::Create(Succ, TrueBranch);
    Instruction *NewInst = createFastInstr(TrueBranch, CurInst, CurDbg);

    Instruction *OriginBr = SplitBB->getTerminator();
    IRBuilder<> BuilderBr(OriginBr);
    BuilderBr.CreateCondBr(CondVal, TrueBranch, FalseBranch);
    OriginBr->eraseFromParent();
    FalseBranch->getTerminator()->setDebugLoc(CurDbg);
    TrueBranch->getTerminator()->setDebugLoc(CurDbg);
    handleSuccPhi(CurInst, FalseBranch, NewInst, TrueBranch, Succ);
    return Succ;
  }

  void replaceBarrier() {
    if (Barriers.empty())
      return;
    // cal in state
    for (auto &Info : BBInfosMap) {
      // use pre bb's gc state value
      auto &BBInfo = Info.second;
      auto BB = Info.first;
      calculateBBCheck(BB, BBInfo);
    }

    for (unsigned i = 0; i < Barriers.size();) {
      BasicBlock *CurBB = Barriers[i]->getParent();
      auto &Info = BBInfosMap[CurBB];
      Instruction *CR = Info.InCheck;
      do {
        if (!CR) {
          CR = createCheck(Barriers[i]);
        }
        CallBase *CI = Barriers[i];
        CurBB = SplitFastPathAndSlowPath(CI, CurBB, CR);
        i++;
        if (i == Barriers.size() || CurBB != Barriers[i]->getParent()) {
          break;
        }
        resetLastCheckResult(CR, CurBB, i);
      } while (true);
    }
  }

  void calculateBBCheck(BasicBlock *BB, BBInfo &Info) {
    if (Info.Visited || (Info.HasCalls && !Info.BeforeCall)) {
      return;
    }
    Info.Visited = true;
    SmallVector<Instruction *, 8> PhiValues;
    for (auto CurBB : Info.PreBBs) {
      auto &CurInfo = BBInfosMap[CurBB];
      if (!CurInfo.HasCalls) {
        calculateBBCheck(CurBB, CurInfo);
      }
      // no barrier, and pre BB has no last barrier
      if (CurInfo.OutCheck == nullptr) {
        PhiValues.clear();
        break;
      } else {
        PhiValues.push_back(CurInfo.OutCheck);
      }
    }
    // update outcheck
    if (PhiValues.size() > 1) {
      Info.InCheck = createPhi(BB, PhiValues);
    } else if (PhiValues.size() == 1) {
      Info.InCheck = PhiValues[0];
    } else if (!Info.HasCalls && Info.LastBarrier != nullptr) {
      Info.InCheck = createCheck(Info.LastBarrier);
    }
    if (!Info.HasCalls) {
      Info.OutCheck = Info.InCheck;
    }
  }
};

class ReadBarrier {
public:
  explicit ReadBarrier(Function &F) : M(F.getParent()), C(F.getContext()) {
    const Triple TT(M->getTargetTriple());
    IsX86_64 = TT.getArch() == Triple::x86_64;
  }

  ~ReadBarrier() = default;

  void readFastPath(CallInst *ReadBarrier, Value *RefFieldPtr,
                    uint64_t Order = 0) {
    setBarrier(ReadBarrier);
    // %0 = load i8 addrspace1*, i8 addrspace1* addrspace1* %3
    // %1 = ptrtoint i8 addrspace1* %0 to i64
    IRBuilder<> Builder(ReadInst);
    // Place may carry colour (same as createStoreOrMems write places).
    LoadInst *Load =
        loadTaggedPointer(Builder, RefFieldPtr, Order);
    Instruction *PtrToInt =
        cast<Instruction>(Builder.CreatePtrToInt(Load, Type::getInt64Ty(C)));
    PtrToInt->setDebugLoc(*Loc);
    Value *CmpEQ = cmpTaggedPointer(PtrToInt, Builder);
    if (ReadBarrier->getIntrinsicID() == Intrinsic::cj_gcread_ref) {
      // A value-type constructor can receive stack ($BP=0), global ($BP=1),
      // or heap storage. Only the heap owner uses this inline heap fast path.
      Value *Owner = Builder.CreatePtrToInt(getBaseObj(ReadBarrier), Type::getInt64Ty(C));
      Value *HeapOwner = Builder.CreateICmpUGT(Owner, Builder.getInt64(1), "cj.read.heap.owner");
      CmpEQ = Builder.CreateAnd(CmpEQ, HeapOwner, "cj.read.heap.fast");
      Type *I64Ty = Type::getInt64Ty(C);
      Value *PlaceI = Builder.CreatePtrToInt(RefFieldPtr, I64Ty, "cj.read.place.i");
      Value *InHeap = emitReservedHeapSlot(Builder, M, PlaceI, "cj.read.inheap");
      CmpEQ = Builder.CreateAnd(CmpEQ, InHeap, "cj.read.heap.slot");
    }
    splitFastPathAndSlowPath(ReadInst->getParent(), CmpEQ, PtrToInt);
  }

  // insert a load from RefFieldPtr:
  //   %val = load i8 addrspace1*, i8 addrspace1* addrspace1* %RefFieldPtr
  LoadInst *loadTaggedPointer(IRBuilder<> &Builder, Value *RefFieldPtr,
                              uint64_t Order) {
    LoadInst *Load = Builder.CreateLoad(DstTy, RefFieldPtr);
    Load->setDebugLoc(*Loc);
    Load->setMetadata("cj.colored.value", MDNode::get(C, {}));
    if (Order) {
      Load->setAtomic(
          (AtomicOrdering)(Order + (uint64_t)AtomicOrdering::Monotonic));
    }
    return Load;
  }

  // Phase B of the ZGC-style colouring work (ops/design/G1_WRITE_BARRIER_DESIGN.md §3.6).
  //
  // This used to be `lshr 48; icmp eq 0`, which asks "are the top 16 bits zero" and so hard-codes
  // "a good reference carries no colour". That is what stops a good colour from being flipped
  // lazily, and therefore what forces the runtime's full-heap extermination walk. Test against a
  // mask the runtime owns instead, so a later phase can give good a non-zero value and flip it at
  // a phase boundary -- ZGC does the same with ZPointerLoadBadMask (zBarrier.inline.hpp:626-628).
  //
  // The runtime owns the mask and republishes it at every phase boundary
  // (WCollector.h set_good_masks), so the provider is not optional: a build
  // without `g_cjLoadBadMask` is a build against a runtime this compiler does
  // not target. The declaration is therefore strong and the load unconditional
  // -- a missing provider must fail at load time, not silently fall back to a
  // predicate that cannot see a flipped good colour.
  //
  // %mask = load i64, i64* @g_cjLoadBadMask
  // %bad  = and i64 %Ptr, %mask
  // %ret  = icmp eq i64 %bad, 0
  Value *cmpTaggedPointer(Value *TagPtr, IRBuilder<> &Builder) {
    Type *I64 = Type::getInt64Ty(C);
    Constant *MaskGV = M->getOrInsertGlobal("g_cjLoadBadMask", I64);
    Value *Mask = Builder.CreateLoad(I64, MaskGV, "cj.loadbadmask");
    cast<Instruction>(Mask)->setDebugLoc(*Loc);
    Value *Bad = Builder.CreateAnd(TagPtr, Mask);
    cast<Instruction>(Bad)->setDebugLoc(*Loc);
    Value *CmpEQ = Builder.CreateICmpEQ(Bad, ConstantInt::get(I64, (uint64_t)0));
    cast<Instruction>(CmpEQ)->setDebugLoc(*Loc);
    return CmpEQ;
  }

  // preBB:
  //   %Cond = icmp eq i64 %tag, 0
  //   br i1 %Cond, label %gcNoMarked label %gcMarked
  // gcNoMarked:
  //   %address = lshr i64 %PtrToInt, @g_cjLoadShift
  //   %val1 = inttoptr i64 %address to i8 addrspace(1)*
  //   br label %loadFinish
  // gcMarked:
  //   %val2 = call @llvm.cj.gcread.ref
  //   br label %loadFinish
  // loadFinish:
  //   %val = phi [%val1, gcNoMarked], [%val2, gcMarked]
  void splitFastPathAndSlowPath(BasicBlock *SplitBB, Value *Condition,
                                Instruction *PtrToInt) {
    BasicBlock *FalseBranch = SplitBB->splitBasicBlock(ReadInst, "gcMarked");
    BasicBlock *Succ =
        FalseBranch->splitBasicBlock(ReadInst->getNextNode(), "loadFinish");
    BasicBlock *TrueBranch =
        BasicBlock::Create(C, "gcNoMarked", SplitBB->getParent(), Succ);
    BranchInst::Create(Succ, TrueBranch);
    IRBuilder<> Builder(TrueBranch->getTerminator());
    // ZPointer::uncolor, zAddress.inline.hpp:609-614. The fast path has
    // established the current remap epoch, so use its published load shift.
    Type *I64 = Type::getInt64Ty(C);
    Constant *ShiftGV = M->getOrInsertGlobal("g_cjLoadShift", I64);
    Value *Shift = Builder.CreateLoad(I64, ShiftGV, "cj.load.shift");
    Value *Address = Builder.CreateLShr(PtrToInt, Shift, "cj.load.address");
    cast<Instruction>(Address)->setDebugLoc(*Loc);
    Instruction *Uncolored =
        cast<Instruction>(Builder.CreateIntToPtr(Address, DstTy));
    Uncolored->setDebugLoc(*Loc);
    Instruction *OriginBr = SplitBB->getTerminator();
    IRBuilder<> BuilderBr(OriginBr);
    BuilderBr.CreateCondBr(Condition, TrueBranch, FalseBranch);
    OriginBr->eraseFromParent();
    TrueBranch->getTerminator()->setDebugLoc(*Loc);
    FalseBranch->getTerminator()->setDebugLoc(*Loc);
    handleSuccPhi(FalseBranch, Uncolored, TrueBranch, Succ);
    return;
  }

  PHINode *handleSuccPhi(BasicBlock *FalseBranch, Value *FastInst,
                         BasicBlock *TrueBranch, BasicBlock *Succ) {
    IRBuilder<> Builder(Succ->getFirstNonPHI());
    PHINode *Phi = Builder.CreatePHI(DstTy, 2);
    ReadInst->replaceAllUsesWith(Phi);
    Phi->addIncoming(ReadInst, FalseBranch);
    Phi->addIncoming(FastInst, TrueBranch);
    return Phi;
  }

  void setBarrier(CallInst *CI) {
    ReadInst = CI;
    DstTy = CI->getType();
    Loc = &CI->getDebugLoc();
  }

private:
  Module *M;
  LLVMContext &C;
  bool IsX86_64;
  CallInst *ReadInst = nullptr;
  Type *DstTy = nullptr;
  const DebugLoc *Loc = nullptr;
};

// ZGC store_barrier_on_heap_oop_field (zBarrier.inline.hpp:695-706) plus
// x86 emit_store_fast_path_check (zBarrierSetAssembler_x86.cpp:358-374).
//
// ZGC color_store_good (zBarrier.inline.hpp:448-450 /
// zAddress.inline.hpp:806-808). Hit arm peels new, ORs StoreGood, i64 store.
// Null is store-good colored. Non-heap owners and mask misses go to MCC. The hit arm
// hands the pre-store word to the runtime exit: like ZGC's load_atomic(p)
// capture (zBarrier.inline.hpp:695-706), this is the SATB deletion record and
// cannot be reconstructed from the installed new word.
class WriteBarrier {
public:
  explicit WriteBarrier(Function &F) : M(F.getParent()), C(F.getContext()) {}

  void storeFastPath(CallInst *CI) {
    Value *NewVal = getValueArg(CI);
    Value *FieldPtr = getPointerArg(CI);
    const DebugLoc &DL = CI->getDebugLoc();
    Type *I64 = Type::getInt64Ty(C);

    IRBuilder<> Builder(CI);
    Value *Place = FieldPtr;
    LoadInst *Prev = Builder.CreateLoad(NewVal->getType(), Place, "cj.store.prev");
    Prev->setDebugLoc(DL);
    Value *PrevI = Builder.CreatePtrToInt(Prev, I64, "cj.store.prev.i");
    cast<Instruction>(PrevI)->setDebugLoc(DL);

    Constant *MaskGV = M->getOrInsertGlobal("g_cjStoreBadMask", I64);
    Value *Mask = Builder.CreateLoad(I64, MaskGV, "cj.storebadmask");
    cast<Instruction>(Mask)->setDebugLoc(DL);
    Value *Bad = Builder.CreateAnd(PrevI, Mask, "cj.store.bad");
    cast<Instruction>(Bad)->setDebugLoc(DL);
    Value *ColourOk =
        Builder.CreateICmpEQ(Bad, ConstantInt::get(I64, (uint64_t)0),
                             "cj.store.colourok");
    cast<Instruction>(ColourOk)->setDebugLoc(DL);
    Value *Owner = Builder.CreatePtrToInt(getBaseObj(CI), I64);
    Value *HeapOwner = Builder.CreateICmpUGT(Owner, Builder.getInt64(1), "cj.store.heap.owner");
    ColourOk = Builder.CreateAnd(ColourOk, HeapOwner, "cj.store.heap.fast");
    Value *PlaceI = Builder.CreatePtrToInt(Place, I64, "cj.store.place.i");
    Value *InHeap = emitReservedHeapSlot(Builder, M, PlaceI, "cj.store.inheap");
    ColourOk = Builder.CreateAnd(ColourOk, InHeap, "cj.store.heap.slot");

    // Then = color_store_good + bare store; Else = MCC.
    // Split at CI so the gcwrite starts Tail; move it into Else.
    Instruction *ThenTerm = nullptr;
    Instruction *ElseTerm = nullptr;
    SplitBlockAndInsertIfThenElse(ColourOk, CI, &ThenTerm, &ElseTerm);
    ThenTerm->setDebugLoc(DL);
    ElseTerm->setDebugLoc(DL);
    ThenTerm->getParent()->setName("storeFinish");
    ElseTerm->getParent()->setName("gcStoreBad");

    // Copy oop bits to an early-clobber i64; peel+OR StoreGood on that
    // i64 only (zAddress.inline.hpp:806). Do not ptrmask/ptrtoint NewVal.
    IRBuilder<> FastBuilder(ThenTerm);
    FunctionType *CopyTy = FunctionType::get(I64, {NewVal->getType()}, false);
    InlineAsm *CopyAsm = InlineAsm::get(CopyTy, "movq $1, $0", "=&r,r",
                                        /*hasSideEffects=*/false);
    Value *NewBits =
        FastBuilder.CreateCall(CopyAsm, NewVal, "cj.store.new.bits");
    cast<CallInst>(NewBits)->setDebugLoc(DL);
    Constant *ShiftGV = M->getOrInsertGlobal("g_cjLoadShift", I64);
    Value *Shift = FastBuilder.CreateLoad(I64, ShiftGV, "cj.store.shift");
    Value *NewI = FastBuilder.CreateShl(NewBits, Shift, "cj.store.new.i");
    cast<Instruction>(NewI)->setDebugLoc(DL);
    Constant *GoodGV = M->getOrInsertGlobal("g_cjStoreGoodMask", I64);
    Value *GoodMask = FastBuilder.CreateLoad(I64, GoodGV, "cj.storegoodmask");
    cast<Instruction>(GoodMask)->setDebugLoc(DL);
    Value *ColoredI =
        FastBuilder.CreateOr(NewI, GoodMask, "cj.store.colored.i");
    cast<Instruction>(ColoredI)->setDebugLoc(DL);
    // ZAddress::store_good, zAddress.inline.hpp:806-808: null is colored too.
    Value *Word = ColoredI;
    unsigned PlaceAS = Place->getType()->getPointerAddressSpace();
    Value *PlaceI64 = FastBuilder.CreateBitCast(
        Place, PointerType::get(I64, PlaceAS), "cj.store.place.i64");
    if (auto *BC = dyn_cast<Instruction>(PlaceI64))
      BC->setDebugLoc(DL);
    StoreInst *St =
        FastBuilder.CreateStore(Word, PlaceI64, /*isVolatile=*/true);
    St->setDebugLoc(DL);
    Value *BaseObj = getBaseObj(CI);
    FunctionType *PostStoreTy = FunctionType::get(
        Type::getVoidTy(C),
        {NewVal->getType(), BaseObj->getType(), FieldPtr->getType(), I64}, false);
    FunctionCallee PostStore =
        M->getOrInsertFunction("CJ_MCC_PostWriteRefField", PostStoreTy);
    CallInst *PostStoreCall = FastBuilder.CreateCall(
        PostStore, {NewVal, BaseObj, FieldPtr, PrevI});
    PostStoreCall->setDebugLoc(DL);
    CI->moveBefore(ElseTerm);
  }

  Module *M;
  LLVMContext &C;
};

/// CJBarrierLowering - This pass rewrites calls to the llvm.gcread or
/// llvm.gcwrite intrinsics, replacing them with simple loads and stores as
/// directed by the GCStrategy. It also performs automatic root initialization
/// and custom intrinsic lowering.
class CJBarrierLowering : public FunctionPass {
  bool isCJBarrier(Instruction *I);
  Instruction *createReadFastPath(BasicBlock *TrueBranch, Instruction *PtrToInt,
                                  const DebugLoc &Loc);
  PHINode *handleSuccPhi(Instruction *SlowInst, BasicBlock *FalseBranch,
                         Instruction *FastInst, BasicBlock *TrueBranch,
                         BasicBlock *Succ);
  void splitFastPathAndSlowPath(CallBase *ReadInst, BasicBlock *SplitBB,
                                Value *Condition, Instruction *PtrToInt,
                                const DebugLoc &Loc);
  void writeBarrierFastPath(Function &F, SetVector<CallInst *> &Barriers);
  void readBarrierFastPath(Function &F, SetVector<CallInst *> &Barriers);
  void doLowering(Function &F);

public:
  static char ID;
  CodeGenOpt::Level OptLevel;

  CJBarrierLowering(CodeGenOpt::Level OptLevel = CodeGenOpt::Default);
  StringRef getPassName() const override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool doInitialization(Module &M) override;
  bool runOnFunction(Function &F) override;
};
} // namespace

// -----------------------------------------------------------------------------

INITIALIZE_PASS_BEGIN(CJBarrierLowering, "cj-barrier-lowering",
                      "Cangjie Barrier Lowering", false, false)
INITIALIZE_PASS_END(CJBarrierLowering, "cj-barrier-lowering",
                    "Cangjie Barrier Lowering", false, false)

char CJBarrierLowering::ID = 0;
char &llvm::CJBarrierLoweringID = CJBarrierLowering::ID;

FunctionPass *llvm::createCJBarrierLoweringPass(CodeGenOpt::Level OptLevel) {
  return new CJBarrierLowering(OptLevel);
}

CJBarrierLowering::CJBarrierLowering(CodeGenOpt::Level OptLevel)
    : FunctionPass(ID), OptLevel(OptLevel) {
  initializeCJBarrierLoweringPass(*PassRegistry::getPassRegistry());
}

StringRef CJBarrierLowering::getPassName() const {
  return "Cangjie Lower Garbage Collection Barrier Function";
}

void CJBarrierLowering::getAnalysisUsage(AnalysisUsage &AU) const {
  FunctionPass::getAnalysisUsage(AU);
}

/// doInitialization - If this module uses the GC intrinsics, find them now.
bool CJBarrierLowering::doInitialization(Module &M) {
  bool HasCangjieFunction = false;
  for (const Function &F : M) {
    if (F.hasCangjieGC()) {
      HasCangjieFunction = true;
      break;
    }
  }
  if (!HasCangjieFunction)
    return false;

  Function *GCStateCheckFunc = M.getFunction("GetGCPhase");
  if (GCStateCheckFunc != nullptr) { // have GetGCPhase in module
    return false;
  }
  LLVMContext &C = M.getContext();
  FunctionType *FuncType = FunctionType::get(Type::getInt32Ty(C), false);
  GCStateCheckFunc =
      cast<Function>(M.getOrInsertFunction("GetGCPhase", FuncType).getCallee());
  GCStateCheckFunc->addFnAttr(Attribute::get(C, "gc-leaf-function"));
  GCStateCheckFunc->addFnAttr(Attribute::get(C, "cj-runtime"));
  GCStateCheckFunc->setCallingConv(CallingConv::CangjieGC);
  GCStateCheckFunc->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
  return false;
}

static bool fastBarrierInline(Function &F, GCPhaseCheck &GCPhase) {
  bool Changed = false;
  GCPhase.initBBInfo(F);
  for (BasicBlock &BB : F) {
    for (auto It = BB.begin(), E = BB.end(); It != E;) {
      auto *CI = dyn_cast<CallBase>(&*It++);
      if (!CI) {
        continue;
      }
      if (mayBeSafepoint(CI)) {
        GCPhase.setHasCall(&BB);
      }
      unsigned IID = CI->getIntrinsicID();
      switch (IID) {
      default:
        break;
      case Intrinsic::cj_gcwrite_ref:
      case Intrinsic::cj_gcwrite_static_ref:
      case Intrinsic::cj_gcwrite_struct:
      case Intrinsic::cj_gcwrite_static_struct:
        if (DisableGCSupport || EnableSafepointOnly) {
          IRBuilder<> Builder(CI);
          createStoreOrMems(CI, Builder);
          CI->eraseFromParent();
          Changed = true;
        } else {
          Changed |= GCPhase.fastBarrier(IID, CI);
        }
        continue;
      case Intrinsic::cj_array_copy_ref:
      case Intrinsic::cj_array_copy_struct:
      case Intrinsic::cj_atomic_store:
        Changed |= GCPhase.fastBarrier(IID, CI);
        continue;
      case Intrinsic::cj_atomic_swap:
      case Intrinsic::cj_atomic_compare_swap:
        // Do not implement fastpath currently now!
        continue;
      }
    }
    GCPhase.updateBBOut(&BB);
  }
  GCPhase.finishAll();
  return Changed;
}

void CJBarrierLowering::writeBarrierFastPath(Function &F,
                                             SetVector<CallInst *> &Barriers) {
  if (EnableTaggedPointer && !CangjieJIT) {
    WriteBarrier WB(F);
    for (CallInst *CI : Barriers) {
      if (!CI->getParent())
        continue;
      if (CI->getIntrinsicID() != Intrinsic::cj_gcwrite_ref)
        continue;
      if (isa<ConstantPointerNull>(getBaseObj(CI)))
        continue;
      WB.storeFastPath(CI);
    }
  }

  if (!EnableGCPhase || CangjieJIT)
    return;

  GCPhaseCheck GCPhase(F);
  fastBarrierInline(F, GCPhase);
}

// do the following conversion:
//   %val = call void @llvm.cj.gcread.ref
// =====>
//   %0 = load i8 addrspace1*, i8 addrspace1* addrspace1* %RefFieldPtr
//   %1 = ptrtoint i8 addrspace1* %0 to i64
//   %2 = and i64 %1, @g_cjLoadBadMask   (phase B; was `lshr i64 %1, 48`)
//   %3 = icmp eq i64 %2, 0
//   br i1 %3, label %gcNoMarked label %gcMarked
// gcNoMarked:
//   %val1 = lshr %0, @g_cjLoadShift
//   br label %loadFinish
// gcMarked:
//   %val2 = call @llvm.cj.gcread.ref
//   br label %loadFinish
// loadFinish:
//   %val = phi [%val1, gcNoMarked], [%val2, gcMarked]
void CJBarrierLowering::readBarrierFastPath(Function &F,
                                            SetVector<CallInst *> &Barriers) {
  if (!EnableTaggedPointer || CangjieJIT)
    return;

  ReadBarrier RB(F);
  for (CallInst *CI : Barriers) {
    if (!CI->getParent())
      continue;
    unsigned ID = CI->getIntrinsicID();
    if (ID == Intrinsic::cj_gcread_ref ||
        ID == Intrinsic::cj_gcread_static_ref) {
      RB.readFastPath(CI, getPointerArg(CI));
      continue;
    }
    if (ID == Intrinsic::cj_atomic_load) {
      if (auto AO = dyn_cast<ConstantInt>(getAtomicOrder(CI))) {
        RB.readFastPath(CI, CI->getArgOperand(AtomicLoad::Field),
                        AO->getZExtValue());
      }
    }
  }
}

void CJBarrierLowering::doLowering(Function &F) {
  Module *M = F.getParent();
  CJStructTypeGCInfo GCInfo(*M);
  BarrierMaker GCBarrier(*M, GCInfo);

  for (BasicBlock &BB : F) {
    for (Instruction &I : llvm::make_early_inc_range(BB)) {
      if (isCJBarrier(&I))
        GCBarrier.replaceInstWithBarrier(cast<IntrinsicInst>(&I));
    }
  }
}

bool CJBarrierLowering::isCJBarrier(Instruction *I) {
  IntrinsicInst *CI = dyn_cast<IntrinsicInst>(I);
  if (!CI)
    return false;

  switch (CI->getIntrinsicID()) {
  default:
    return false;
  case Intrinsic::cj_gcwrite_ref:
  case Intrinsic::cj_gcwrite_struct:
  case Intrinsic::cj_gcwrite_static_ref:
  case Intrinsic::cj_gcwrite_static_struct:
  case Intrinsic::cj_gcread_ref:
  case Intrinsic::cj_gcread_weakref:
  case Intrinsic::cj_gcread_struct:
  case Intrinsic::cj_gcread_static_ref:
  case Intrinsic::cj_gcread_static_struct:
  case Intrinsic::cj_copy_struct_field:
  case Intrinsic::cj_atomic_store:
  case Intrinsic::cj_atomic_load:
  case Intrinsic::cj_atomic_swap:
  case Intrinsic::cj_atomic_compare_swap:
  case Intrinsic::cj_array_copy_ref:
  case Intrinsic::cj_array_copy_struct:
  case Intrinsic::cj_array_copy_generic:
  case Intrinsic::cj_assign_generic:
  case Intrinsic::cj_gcwrite_generic:
  case Intrinsic::cj_gcread_generic:
  case Intrinsic::cj_gcwrite_generic_payload:
  case Intrinsic::cj_gcread_generic_payload:
    return true;
  }
}

static void replaceFastFunc(Function &F, GCStatepointInst *CI,
                            StringRef FuncName) {
  Module *M = F.getParent();
  Function *Func = M->getFunction(FuncName);
  if (Func == nullptr) {
    Function *Callee = CI->getActualCalledFunction();
    Func = M->declareCJRuntimeFunc(FuncName, Callee->getFunctionType(), true);
    Func->setLinkage(GlobalValue::InternalLinkage);
    Func->addFnAttr(Attribute::NoInline);
    Func->addFnAttr("cj-fast-new-obj");
    BasicBlock *BB = BasicBlock::Create(M->getContext(), "entry", Func);
    IRBuilder<> IRB(BB);
    auto Ret = IRB.CreateCall(Callee, {Func->getArg(0), Func->getArg(1)});
    IRB.CreateRet(Ret);
  }
  CI->setArgOperand(GCStatepointInst::CalledFunctionPos, Func);
}

static bool doNewFastPath(Function &F, SetVector<GCStatepointInst *> &NewObjs) {
  if (CangjieJIT)
    return false;
  for (GCStatepointInst *CI : NewObjs) {
    Function *Callee = CI->getActualCalledFunction();
    if (Callee->getName().equals("CJ_MCC_NewObject")) {
      replaceFastFunc(F, CI, NewObjFastStr);
    } else {
      replaceFastFunc(F, CI, NewObjFinalizerFastStr);
    }
  }
  return true;
}

static bool isNewObj(Instruction *I) {
  if (auto CI = dyn_cast<GCStatepointInst>(I)) {
    Function *Callee = CI->getActualCalledFunction();
    if (Callee && Callee->getName().isCangjieNewObjFunction()) {
      return true;
    }
  }
  return false;
}

static bool isSafepointCall(Instruction *I) {
  if (auto *CI = dyn_cast<GCStatepointInst>(I)) {
    Function *Callee = CI->getActualCalledFunction();
    if (Callee && Callee->isCangjieSafePoint())
      return true;
  }
  return false;
}

static Function *getOrInsertSafepointStub(Module *M, Function *Callee) {
  if (Function *F = M->getFunction(SafepointStub))
    return F;
  FunctionType *FuncType =
      FunctionType::get(Type::getVoidTy(M->getContext()), false);
  Function *F = cast<Function>(
      M->getOrInsertFunction(SafepointStub, FuncType).getCallee());
  F->addFnAttr(Attribute::NoInline);
  F->addFnAttr(Attribute::Naked);
  F->setLinkage(GlobalValue::PrivateLinkage);
  // Create function body.
  BasicBlock *BB = BasicBlock::Create(M->getContext(), "", F);
  IRBuilder<> IRB(BB);
  IRB.CreateCall(Callee);
  // Ret does not need to be generated, but the verification fails.
  IRB.CreateRetVoid();
  return F;
}

static bool combineSafepointStub(Module *M,
                                 SetVector<GCStatepointInst *> &Safepoints) {
  if (Safepoints.empty())
    return false;
  Function *F = getOrInsertSafepointStub(
      M, Safepoints.front()->getActualCalledFunction());
  for (auto *SI : Safepoints) {
    // Although the callee is replaced, the call is still a safepoint.
    SI->setArgOperand(GCStatepointInst::CalledFunctionPos, F);
    SI->setArgOperand(GCStatepointInst::IDPos,
                      ConstantInt::get(Type::getInt64Ty(M->getContext()),
                                       Cangjie::CJStatepointID::SafepointStub));
  }
  return true;
}

// llvm.cj.copy.no.ref.struct has already had its concrete AggType and exact
// size checked by CJIRVerifier.  It carries no reference slots and therefore
// bypasses CJBarrierSplit and all runtime barrier entry points.  Restore the
// original byte-copy operation immediately before ordinary CodeGen.
static bool lowerNoReferenceStructCopies(Function &F) {
  SmallVector<IntrinsicInst *, 4> Copies;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *II = dyn_cast<IntrinsicInst>(&I))
        if (II->getIntrinsicID() == Intrinsic::cj_copy_no_ref_struct)
          Copies.push_back(II);

  for (IntrinsicInst *II : Copies) {
    IRBuilder<> Builder(II);
    CallInst *Copy = Builder.CreateMemCpy(
        II->getArgOperand(0), II->getParamAlign(0), II->getArgOperand(1),
        II->getParamAlign(1), II->getArgOperand(2), /*isVolatile=*/false);
    Copy->copyMetadata(*II);
    Copy->setMetadata(LLVMContext::MD_cj_agg, nullptr);
    Copy->setDebugLoc(II->getDebugLoc());
    II->eraseFromParent();
  }
  return !Copies.empty();
}

bool CJBarrierLowering::runOnFunction(Function &F) {
  bool Changed = lowerNoReferenceStructCopies(F);
  // Quick exit for functions that do not use Cangjie GC.
  if (!F.hasCangjieGC())
    return Changed;

  const Triple TT(F.getParent()->getTargetTriple());
  if (TT.isARM()){
    EnableTaggedPointer = false;
    EnableGCPhase = false;
    EnableGCFastPath = false;
  }

  SetVector<CallInst *> Barriers;
  SetVector<GCStatepointInst *> News;
  SetVector<GCStatepointInst *> Safepoints;

  for (BasicBlock &BB : F) {
    for (Instruction &I : llvm::make_early_inc_range(BB)) {
      if (isCJBarrier(&I))
        Barriers.insert(cast<CallInst>(&I));
      else if (EnableGCFastPath && isNewObj(&I))
        News.insert(cast<GCStatepointInst>(&I));
      else if (EnableSafepointOutline && isSafepointCall(&I))
        Safepoints.insert(cast<GCStatepointInst>(&I));
    }
  }

  if (!News.empty()) {
    Changed = doNewFastPath(F, News);
  }

  if (!Safepoints.empty() && !TT.isARM())
    Changed |= combineSafepointStub(F.getParent(), Safepoints);

  // Independent of barrier set: bare CreateMemMove has no CJ barrier intrinsic.
  // Independent of barrier set: value-struct field load/store is a raw
  // LoadInst/StoreInst, not llvm.cj.gcread/gcwrite.

  if (Barriers.empty()) {
    return Changed;
  }

  // Store-good paint needs the load peel at every opt level, -O0 included
  // (zAddress.inline.hpp:609).
  readBarrierFastPath(F, Barriers);
  writeBarrierFastPath(F, Barriers);
  doLowering(F);
  return true;
}
