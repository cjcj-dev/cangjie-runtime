// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Base/Types.h"
#include "Common/TypeDef.h"
#include <cstring>
#include <map>
#if defined(_WIN64)
#define NOGDI
#include <windows.h>
#endif
#include "Heap/z/zMark.hpp"
#include "Common/ScopedObjectAccess.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/z/zReferenceProcessor.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zUncoloredRoot.hpp"
#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "ObjectModel/RefField.inline.h"
#include "MutatorManager.h"
#include "StackManager.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ExceptionManager.h"
#include "schedule.h"
#ifdef _WIN64
#include "WinModuleManager.h"
#endif
#include "CpuProfiler/CpuProfiler.h"
#include "Base/LogFile.h"
#include "Interpreter/InterpreterSpecific.h"
#include "Loader/ElfUnloadQuiescence.h"

namespace MapleRuntime {
extern "C" uintptr_t MRT_GetThreadLocalData()
{
    uintptr_t tlDataAddr = reinterpret_cast<uintptr_t>(ThreadLocal::GetThreadLocalData());
#if defined(__aarch64__)
    if (Heap::GetHeap().IsGcStarted()) {
        const Mutator* mutator = Mutator::GetMutator();
        // Since the TBI(top bit ignore) feature in Aarch64,
        // set gc phase to high 8-bit of ThreadLocalData Address for gc barrier fast path.
        // 56: make gcphase value shift left 56 bit to set the high 8-bit
        (void)mutator;
    }
#endif
    return tlDataAddr;
}

extern "C" bool MRT_EnterSaferegion(bool updateUnwindContext)
{
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr) {
        return false;
    }
    return mutator->EnterSaferegion(updateUnwindContext);
}

extern "C" bool MRT_LeaveSaferegion()
{
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr) {
        return false;
    }
    return mutator->LeaveSaferegion();
}

extern "C" void MRT_SetGrowFlag(bool flag)
{
    if (!CJThreadSetStackGrow(flag)) {
        return;
    }
    LOG(RTLOG_ERROR, "Set flag of GROWStack faild.");
}

extern "C" intptr_t MRT_StackGrow(intptr_t frameBase, uint32_t adjustedSize, void* ip)
{
    // arm32 only do stack check and try to throw StackOverFlow Expection.
#ifdef __arm__
    if (adjustedSize != 0) {
        LOG(RTLOG_FAIL, "Unsupported stack grow for arm32");
    }
    uintptr_t threadData = MRT_GetThreadLocalData();
    uint32_t protectAddr = reinterpret_cast<uint32_t>(reinterpret_cast<ThreadLocalData*>(threadData)->protectAddr);
    // for runtime we could not add sp asm, keep a PRESERVE_SIZE to avoid stepping on illegal memory
    constexpr uint32_t PRESERVE_SIZE = 256;
    if (protectAddr >= frameBase - PRESERVE_SIZE) {
        void* fa = __builtin_frame_address(0);
        static_cast<FrameAddress*>(fa)->returnAddress = static_cast<uint32_t*>(ip);
        ExceptionManager::StackOverflow(adjustedSize, ip);
        return 0;
    }
    return 0;
#else
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr) {
        return false;
    }
    return mutator->FixExtendedStack(frameBase, adjustedSize, ip);
#endif
}

extern "C" void MRT_FreeOldStack(intptr_t offset)
{
    if (offset == 0) { return; }
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr) {
        return;
    }
    CJThreadOldStackFree(reinterpret_cast<void*>(mutator->GetStackTopAddr()), mutator->GetStackSize());
    mutator->SetStackTopAddr(reinterpret_cast<uintptr_t>(CJThreadStackAddrGet()));
    mutator->SetStackSize(CJThreadStackSizeGet());
    mutator->SetStackBaseAddr(reinterpret_cast<uintptr_t>(CJThreadStackBaseAddrGet()));
}

extern "C" void MRT_SetStackGrow(bool enableStackScale)
{
    if (ThreadLocal::GetThreadType() == ThreadType::FP_THREAD) {
        return;
    }
    if (!CJThreadSetStackGrow(enableStackScale)) {
        return;
    } else {
#if not defined (__OHOS__) && not defined (_WIN64)
        LOG(RTLOG_ERROR, "CJThread Set StackScale failed");
#endif
    }
}

#ifdef INTERPRETER_ENABLED
void Mutator::InitInterpreterPart()
{
    if (isRuntimeMutator) {
        return;
    }

    DLOG(INTERPRETER, "[Mutator] init interpreter part for cjThread %p\n", this);
    InterpreterCJThreadStart(&(this->interpreterCJThreadData));
}

void Mutator::DestroyInterpreterPart()
{
    if (isRuntimeMutator) {
        return;
    }

    DLOG(INTERPRETER, "[Mutator] destruct interpreter part for cjThread %p\n", this);
    InterpreterCJThreadDestroy(&(this->interpreterCJThreadData));
}
#endif

void Mutator::InitProtectStackAddr()
{
#if defined(_WIN64)
    _TEB* teb = NtCurrentTeb();
    stackBoundAddr = reinterpret_cast<void*>(reinterpret_cast<NT_TIB64*>(teb)->StackLimit);
#elif defined(__APPLE__)
    stackBoundAddr = pthread_get_stackaddr_np(pthread_self());
#else
    pthread_attr_t attr;
    pthread_t thread = pthread_self();
    CHECK_PTHREAD_CALL(pthread_getattr_np, (thread, &attr), "get thread attr failed");
    uintptr_t sSize = 0;
    CHECK_PTHREAD_CALL(pthread_attr_getstack, (&attr, &stackBoundAddr, &sSize), "get thread stack attr failed");
    CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "destroy pthread attr");
#endif
    size_t reversedSize = Runtime::Current().GetConcurrencyModel().GetReservedStackSize();
    ThreadLocal::SetProtectAddr(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(stackBoundAddr) + reversedSize));
}

void Mutator::ResetMutator()
{
    CHECK_DETAIL(nativeFrameRoots.empty(), "native frame roots are not released");
    SetManagedContext(false);
    StorePlain(rawObject, zaddress::null);
    // Exit publishes the logical owner's private work before scheduler
    // unbinding can expose another owner through this OS TLS binding.
    Heap& heap = Heap::GetHeap();
    gcData.storeBarrierBuffer->Flush();
    (void)heap.FlushGCDataMarkProducers(gcData);
    ReleaseAllocBuffer();
    uwContext.Reset();
    // ClearInfo below clears the throwing-SOF marker; pair the stack-guard Recover that
    // BeginCatch would have performed, or the guard stays expanded with nothing left to
    // say so. All three callers run on the mutator's own thread, so recovering here
    // targets the right stack. What it buys differs by caller: for a reusable scheduler
    // cjthread (TransitMutatorToExit) it keeps an expanded guard out of the freelist,
    // and for the runtime mutator it restores the current thread's protect boundary.
    // (The finalizer mutator's setup path never arms that boundary — StackGuardRecover
    // sees the null and leaves the threshold alone — and a finalizer whose exception
    // was fatal aborts before reaching this Reset at all.)
    if (exceptionWrapper.IsThrowingSOFE()) {
        StackGuardRecover();
    }
    exceptionWrapper.ClearInfo();
    // stackwm #1 lifecycle: exit/reset closes watermark (must not leave SCANNING dangling).
    stackWatermark.Reset();
    MutatorUnlock();
}

void Mutator::SetManagedContext(bool isManagedContext)
{
    inManagedContext.store(isManagedContext, std::memory_order_release);
}

void Mutator::HandleSuspensionRequest()
{
    for (;;) {
        Handshake::Current().process_by_self();
        SetInSaferegion(SAFE_REGION_TRUE);
        MarkFlushOnEnterSaferegion();
        if (HasSuspensionRequest(SUSPENSION_FOR_CPU_PROFILE)) {
            TransitionToCpuProfile(true);
        } else if (HasSuspensionRequest(SUSPENSION_FOR_SYNC)) {
            SuspendForSync();
            if (HasSuspensionRequest(SUSPENSION_FOR_CPU_PROFILE)) {
                TransitionToCpuProfile(true);
            }
        } else if (HasPreemptRequest()) {
            SuspendForPreempt();
        } else if (HasSuspensionRequest(SUSPENSION_FOR_EXIT)) {
            while (true) {
                sleep(INT_MAX);
            }
        }
        MarkFlushBeginLeaveSaferegion();
        SetInSaferegion(SAFE_REGION_FALSE);
        MarkFlushEndLeaveSaferegion();
        if (MutatorManager::Instance().SyncTriggered()) {
            // entering this branch means a second request has been broadcasted, we need to reset this flag to avoid
            // missing the request. And this must be after the behaviour that set saferegion state to false, because
            // we need to make sure that the mutator can always perceive the gc request when the mutator is not in
            // safe region.
            SetSuspensionFlag(SUSPENSION_FOR_SYNC);
        }
        // Leave saferegion if current mutator has no suspend request, otherwise try again
        if (LIKELY(!HasAnySuspensionRequest() && !HasObserver())) {
            return;
        }
    }
}

void Mutator::SuspendForSync()
{
    ClearSuspensionFlag(SUSPENSION_FOR_SYNC);
    // wait until StartTheWorld
    int curCount = static_cast<int>(MutatorManager::Instance().GetSyncFutexWordValue());
    // Avoid losing wake-ups
    if (curCount > 0) {
#if defined(_WIN64) || defined(__APPLE__)
        MutatorManager::Instance().MutatorWait();
#else
        int* countAddr = MutatorManager::Instance().GetSyncFutexWord();
        // FUTEX_WAIT may fail when gc thread wakes up all threads before the current thread reaches this position.
        // But it is not important because there won't be data race between the current thread and the gc thread,
        // and it also won't be frozen since gc thread also modifies the value at countAddr before its waking option.
        (void)Futex(countAddr, FUTEX_WAIT, curCount);
#endif
    }
}


// zVerify.cpp:323-342: verify only the roots whose watermark processing
// has started, and never read frames still waiting for processing.
void Mutator::VisitProcessedRoots(const RootVisitor& visitor)
{
    if (!GetStackWatermark().IsDone() && GetStackWatermark().GetEpoch() == 0) { return; }
    VisitExceptionRoots(visitor);
    VisitNativeFrameRoots(visitor);
    if (GetStackWatermark().IsDone()) { VisitStackRoots(visitor, visitor); }
}

void Mutator::VisitStackRoots(const RootVisitor& func, const RootVisitor& invisibleRootVisitor)
{
    MutatorLock();
    const RootVisitor& visitedInvisibleRootVisitor = invisibleRootVisitor;
    // A native/exclusive frame has no managed stack map, but its side roots are
    // independent of stack metadata and must remain visible. In particular an
    // incomplete large reference array can be published while a native helper
    // owns the mutator.
    if (!IsManagedContext()) {
        VisitRawObjects(visitedInvisibleRootVisitor);
        MutatorUnlock();
        return;
    }
    IncObserver();
    StackManager::VisitStackRoots(uwContext, func, *this);
    VisitRawObjects(visitedInvisibleRootVisitor);
    DecObserver();
    MutatorUnlock();
}

void Mutator::VisitExceptionRoots(const RootVisitor& func)
{
    // ExceptionRef is a legacy ABI word owned by ExceptionWrapper; metadata classifies it as a root.
    RootSlot& root = RootSlotAt(&exceptionWrapper.GetExceptionRef());

    func(root);
}

void Mutator::VisitRawObjects(const RootVisitor& func)
{
    // Pairs with PublishInvisibleRoot's release store: a scanner that sees the
    // root must also see the already-published type and array length.
    zaddress_unsafe rootValue = rawObject.LoadPlain(std::memory_order_acquire);
    if (!is_null(rootValue)) {

        func(rawObject);
    }
}

void Mutator::VisitNativeFrameRoots(const RootVisitor& func)
{
    for (ObjectRef& root : nativeFrameRoots) {
        func(root);
    }
}

ObjectRef* Mutator::AddNativeFrameRoot(BaseObject* obj)
{
    nativeFrameRoots.emplace_back();
    StorePlain(nativeFrameRoots.back(), from_object(obj));
    return &nativeFrameRoots.back();
}

void Mutator::PopNativeFrameRootsTo(size_t mark)
{
    if (mark < nativeFrameRoots.size()) {
        nativeFrameRoots.resize(mark);
    }
}

void Mutator::VisitHeapReferencesOnStack(const RootVisitor& rootVisitor, const DerivedPtrVisitor& derivedPtrVisitor,
                                         bool young)
{
    VisitHeapReferencesOnStack(rootVisitor, rootVisitor, derivedPtrVisitor, rootVisitor, young);
}

void Mutator::VisitHeapReferencesOnStack(const RootVisitor& regRootVisitor, const RootVisitor& slotRootVisitor,
                                         const DerivedPtrVisitor& derivedPtrVisitor,
                                         const RootVisitor& rawObjectVisitor, bool young)
{
    MutatorLock();
    // No managed frame means there is no stack map to visit. Side roots are
    // stored outside the managed stack and still require relocation repair.
    if (!IsManagedContext()) {
        VisitRawObjects(rawObjectVisitor);
        MutatorUnlock();
        return;
    }
    IncObserver();
    StackManager::VisitHeapReferencesOnStack(
        uwContext, regRootVisitor, slotRootVisitor, derivedPtrVisitor, *this, young);
    VisitRawObjects(rawObjectVisitor);
    DecObserver();
    MutatorUnlock();
}

void Mutator::VisitHeapReferences(const RootVisitor& rootVisitor, const DerivedPtrVisitor& derivedPtrVisitor,
                                  bool young)
{
    VisitHeapReferencesOnStack(rootVisitor, derivedPtrVisitor, young);
    VisitExceptionRoots(rootVisitor);
    VisitNativeFrameRoots(rootVisitor);
}

void Mutator::VisitHeapReferences(const RootVisitor& regRootVisitor, const RootVisitor& slotRootVisitor,
                                  const DerivedPtrVisitor& derivedPtrVisitor,
                                  const RootVisitor& exceptionRootVisitor, const RootVisitor& rawObjectVisitor,
                                  bool young)
{
    VisitHeapReferencesOnStack(regRootVisitor, slotRootVisitor, derivedPtrVisitor, rawObjectVisitor, young);
    VisitExceptionRoots(exceptionRootVisitor);
    VisitNativeFrameRoots(exceptionRootVisitor);
}

Mutator* Mutator::GetMutator() noexcept
{
    Mutator* mutator = ThreadLocal::GetMutator();
    if (mutator == nullptr) {
        mutator = ConcurrencyModel::GetMutator();
    }
    return mutator;
}

void Mutator::StackGuardExpand() const
{
    // Expand stack boundary when StackOverflowError occurs
    if (!IsRuntimeThread()) {
        CJThreadStackGuardExpand();
        // No own stack (foreign/exclusive): the expand above was a no-op and there is
        // no guard page to unprotect — nullptr minus a page is not an address.
        if (CJThreadStackAddrGet() == nullptr) {
            return;
        }
        if (Runtime::Current().GetConcurrencyModel().GetStackGuardCheckFlag()) {
            void* topAddr = reinterpret_cast<uint8_t*>(CJThreadStackAddrGet()) - MapleRuntime::MRT_PAGE_SIZE;
#ifdef _WIN64
            DWORD oldProt = 0;
            int ret = VirtualProtect(topAddr, MapleRuntime::MRT_PAGE_SIZE, PAGE_READWRITE, &oldProt);
            if (ret == 0) {
                LOG(RTLOG_ERROR, "Enable stack protect page failed");
            }
#else
            int ret = mprotect(topAddr, MapleRuntime::MRT_PAGE_SIZE, PROT_READ | PROT_WRITE);
            if (ret != 0) {
                LOG(RTLOG_ERROR, "Enable stack protect page failed");
            }
#endif
        }
    } else {
        ThreadLocal::SetProtectAddr(static_cast<uint8_t*>(stackBoundAddr));
    }
}

void Mutator::StackGuardRecover() const
{
    // Recover stack boundary when StackOverflowError has been caught
    if (!IsRuntimeThread()) {
        CJThreadStackGuardRecover();
        if (CJThreadStackAddrGet() == nullptr) {
            return;
        }
        if (Runtime::Current().GetConcurrencyModel().GetStackGuardCheckFlag()) {
            void* topAddr = reinterpret_cast<uint8_t*>(CJThreadStackAddrGet()) - MapleRuntime::MRT_PAGE_SIZE;
#ifdef _WIN64
            DWORD oldProt = 0;
            int ret = VirtualProtect(topAddr, MapleRuntime::MRT_PAGE_SIZE, PAGE_NOACCESS, &oldProt);
            if (ret == 0) {
                LOG(RTLOG_ERROR, "Disable stack protect page failed");
            }
#else
            int ret = mprotect(topAddr, MapleRuntime::MRT_PAGE_SIZE, PROT_NONE);
            if (ret != 0) {
                LOG(RTLOG_ERROR, "Disable stack protect page failed");
            }
#endif
        }
    } else {
        // A runtime-thread mutator whose protect boundary was never armed (the
        // finalizer mutator's setup path skips InitProtectStackAddr) has nothing to
        // restore; null + reserved would install a bogus non-null threshold.
        if (stackBoundAddr == nullptr) {
            return;
        }
        size_t reversedSize = Runtime::Current().GetConcurrencyModel().GetReservedStackSize();
        ThreadLocal::SetProtectAddr(
            reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(stackBoundAddr) + reversedSize));
    }
}

void Mutator::InitStackInfo(ThreadLocalData* threadData)
{
    CJThread* cjthread = reinterpret_cast<CJThread*>(threadData->cjthread);
    SetStackTopAddr(reinterpret_cast<uintptr_t>(CJThreadStackAddrGetByCJThrd(cjthread)));
    SetStackSize(CJThreadStackSizeGetByCJThrd(cjthread));
    SetStackBaseAddr(reinterpret_cast<uintptr_t>(CJThreadStackBaseAddrGetByCJThrd(cjthread)));
}

bool Mutator::IsStackAddr(uintptr_t addr)
{
    if (addr > GetStackTopAddr() && addr < GetStackTopAddr() + GetStackSize()) {
        return true;
    } else {
        return false;
    }
}

void Mutator::RecordStackPtrs(std::set<RootSlot*>& rootSlots,
                              std::vector<std::tuple<DerivedSlot*, BasePtrType, size_t>>& derivedSlots)
{
    // The pointer on the stack to be fixed has two sources:
    //     1. the non-escaped heap pointer (from heap stackmap), these pointer are assigned to the stack.
    //     2. the stack pointer (from stack stackmap).
    // The stack pointer and the pointer on the heap obtained after ref trace are placed in the <resSet> for fix.

    // The non-escaped heap pointer points to an object,
    //     so they need to be traced to ensure that all pointers are fixed.
    // These pointers will be collected in the <rootList>.
    std::stack<RootSlot*, std::deque<RootSlot*, StdContainerAllocator<RootSlot*, STACK_PTR>>> rootList;
    StackPtrVisitor traceAndFixPtrVisitor = [&rootList, this](ObjectRef& oldStackAddr) {
        if (IsStackAddr(raw(oldStackAddr.LoadPlain()))) {
            rootList.push(&oldStackAddr);
        }
    };
    // The stack pointer does not require ref trace.
    StackPtrVisitor fixPtrVisitor = [&rootSlots, this](ObjectRef& oldStackAddr) {
        if (IsStackAddr(raw(oldStackAddr.LoadPlain()))) {
            rootSlots.insert(&oldStackAddr);
        }
    };
    // Preserve the pair before stack movement; the only later write is RebaseDerived(newBase, offset).
    DerivedPtrVisitor derivedPtrVisitor =
        [&derivedSlots, this](BasePtrType basePtr, DerivedSlot& derivedPtr) {
        const zaddress_unsafe derivedValue = derivedPtr.LoadDerived();
        if (IsStackAddr(raw(derivedValue))) {
            CHECK_DETAIL(!is_null(basePtr) && raw(derivedValue) >= raw(basePtr),
                         "stack derived pointer must have a non-null base at or below it");
            derivedSlots.emplace_back(&derivedPtr, basePtr, raw(derivedValue) - raw(basePtr));
        }
    };
    StackManager::VisitStackPtrMap(uwContext, traceAndFixPtrVisitor, fixPtrVisitor, derivedPtrVisitor, *this);

    // Ref trace on non-escaped heap pointers.
    HeapSlotVisitor refVisitor = [&rootList, this](HeapSlot<>& oldRefFieldAddr) {
        // A reference field in a stack-allocated object is a root slot, not a heap slot.
        RootSlot& oldRootField = RootSlotAt(
            static_cast<void*>(&oldRefFieldAddr)); // Stack-object field storage is root storage.
        // Check whether the address is on the stack.
        if (IsStackAddr(raw(oldRootField.LoadPlain()))) {
            rootList.push(&oldRootField);
        }
    };
    for (;;) {
        if (rootList.empty()) {
            break;
        }
        // get next object from work stack.
        RootSlot* objSlot = rootList.top();
        rootList.pop();
        rootSlots.insert(objSlot);
        // StackPtrMap only records live stack addresses while the old stack is still mapped.
        BaseObject* obj = to_object(safe(objSlot->LoadPlain()));
        if (!obj->IsValidObject()) {
            continue;
        }

        TypeInfo* tip = obj->GetTypeInfo();
        uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
        CHECK_DETAIL((tipAddr & StateWord::ADDRESS_ALIGN_MASK) == 0,
                     "RecordStackPtrs: TypeInfo %p on stack object %p (slot %p) is not 8-byte aligned "
                     "(stateWord non-zero is not a managed-object proof)",
                     tip, obj, objSlot);
        CHECK_DETAIL(tip->IsVaildType(),
                     "RecordStackPtrs: TypeInfo %p on stack object %p (slot %p) has invalid type kind",
                     tip, obj, objSlot);
        if (!obj->HasRefField()) {
            continue;
        }
        obj->ForEachRefField(refVisitor);
    }
}

intptr_t Mutator::FixExtendedStack(intptr_t frameBase, uint32_t adjustedSize, void* ip)
{
    if (!IsRuntimeThread()) {
#if defined(_WIN64)
        stackGrowFrameSize = adjustedSize;
#endif
        // A no-stack cjthread (foreign/exclusive) copies a null base but a nonzero
        // configured size into this mutator, so the doubling loops below would iterate
        // on wrap-around arithmetic before the guarded allocator ever answered zero.
        // No own stack means no growth to size — but only the arithmetic is skipped,
        // not the branch semantics: the FFI entry (frameBase == 0) answered a plain
        // zero, while both stack-check entries raised StackOverflow when growth
        // failed, and a caller that asked for a stack check must still get its
        // exception.
        if (stackBaseAddr == 0) {
            if (frameBase != 0) {
#ifdef INTERPRETER_ENABLED
                // The interpreter branch below logs its own failure; keep that visible
                // here too, without the frame dereference and doubling loop that only
                // make sense for a stack that exists.
                if (StackManager::IsInterpreterCodeAddr(reinterpret_cast<uintptr_t>(ip))) {
                    DLOG(INTERPRETER, "       stack overflow at %p on a cjthread with no own stack", ip);
                }
#endif
                ExceptionManager::StackOverflow(adjustedSize, ip);
            }
            return 0;
        }
        intptr_t stackOffset;
        // When frameBase is 0, it is actively invoked in the FFI. In this case, the stack is expanded to the maximum.
        // When frameBase != 0, the stack check is invoked. In this case, the stack is expanded by two times by default.
        // Check whether the stack expansion meets the requirements.
        // Otherwise, the stack expansion continues to reach the limit.
#ifdef INTERPRETER_ENABLED
        uintptr_t currentIp = reinterpret_cast<uintptr_t>(ip);
        bool isInInterpreter = StackManager::IsInterpreterCodeAddr(currentIp);
#endif
        if (frameBase == 0) {
            stackOffset = CJThreadStackGrow(CJTHREAD_MAX_STACK_SIZE);
            if (stackOffset == 0 || stackOffset == -1) {
                return 0;
            }
#ifdef INTERPRETER_ENABLED
        } else if (isInInterpreter) {
            // The interpreter stack-grow path passes the full size that must fit below the caller
            // frame base after StackGrowStub passes execution into the prologue of interpreted method.
            DLOG(INTERPRETER, "Stack overflow happened in interpreter, stack size: %zu", stackSize);
            const uintptr_t* stubFrameBase = reinterpret_cast<const uintptr_t*>(frameBase);
            uintptr_t interpFrameBase = *stubFrameBase;

            size_t requiredSp = interpFrameBase - GetFrameSize(interpFrameBase);
            size_t newSize = stackSize + stackSize;
            while (stackBaseAddr - requiredSp > newSize - CJThreadStackReversedGet()) {
                newSize += newSize;
            }
            DLOG(INTERPRETER, "   try to grow stack size: %zu -> %zu", stackSize, newSize);
            stackOffset = CJThreadStackGrow(newSize);
            if (stackOffset == -1 || stackOffset == 0) {
                DLOG(INTERPRETER, "       stack overflow at %p", ip);
                ExceptionManager::StackOverflow(adjustedSize, ip);
                return 0;
            }
#endif // INTERPRETER_ENABLED
        } else {
            UnwindContext& stackGrowContext = Mutator::GetMutator()->GetUnwindContext();
            UnwindContext caller;
#ifdef _WIN64
            UnwindContextStatus ucs = stackGrowContext.GetUnwindContextStatus();
            stackGrowContext.frameInfo.mFrame.UnwindToCallerMachineFrame(caller.frameInfo, ucs);
#else
            stackGrowContext.frameInfo.mFrame.UnwindToCallerMachineFrame(caller.frameInfo.mFrame);
#endif
            caller.frameInfo.ResolveProcInfo();
            ElfUnloadQuiescence::ReadScope metadataReader;
#ifdef __APPLE__
            FuncDescRef funcDesc = MFuncDesc::GetFuncDesc(caller.frameInfo.mFrame.GetFA());
#else
            FuncDescRef funcDesc = MFuncDesc::GetFuncDesc(reinterpret_cast<Uptr>(caller.frameInfo.GetFuncStartPC()));
#endif
            Uptr* stackMapEntry = funcDesc->GetStackMap();
            uint32_t validPos = 0;
            uint32_t frameSize = EHFrameInfo::ReadVarInt(&stackMapEntry, validPos);
#if defined(__x86_64__)
            // 8 is the slot length of returnaddr.
            uint64_t callerSp = *reinterpret_cast<intptr_t*>(frameBase) - frameSize + 8;
#elif defined(__aarch64__)
            uint64_t callerSp = *reinterpret_cast<intptr_t*>(frameBase) - frameSize;
#elif defined(__arm__)
            uint64_t callerSp = *reinterpret_cast<intptr_t*>(frameBase) - frameSize;
#endif
            size_t newSize = stackSize + stackSize;
            while (stackBaseAddr - callerSp > newSize - CJThreadStackReversedGet()) {
                newSize += newSize;
            }
            stackOffset = CJThreadStackGrow(newSize);
            if (stackOffset == -1 || stackOffset == 0) {
                ExceptionManager::StackOverflow(adjustedSize, ip);
                return 0;
            }
        }

        // Visits the stackmap and records all pointers to be fixed to the resSet.
        std::set<RootSlot*> rootSlots;
        std::vector<std::tuple<DerivedSlot*, BasePtrType, size_t>> derivedSlots;
        RecordStackPtrs(rootSlots, derivedSlots);

        // Serialize against VisitStackRoots / concurrent GC stack fill (stackwm #7 Q4):
        // absolute-FA caches must not be built against a half-moved stack.
        MutatorLock();
        // Fix All pointers recorded in resSet.
        for (RootSlot* oldSlot : rootSlots) {
            RootSlot& newSlot = RootSlotAt(reinterpret_cast<MAddress>(oldSlot) + stackOffset);
            // The copied value is a stack address; adding stackOffset yields a committed new-stack address.
            StorePlain(newSlot, to_zaddress(raw(newSlot.LoadPlain()) + stackOffset));
        }
        for (auto& [oldDerivedSlot, oldBase, offset] : derivedSlots) {
            DerivedSlot& newDerivedSlot =
                DerivedSlotAt(reinterpret_cast<MAddress>(oldDerivedSlot) + stackOffset);
            RootSlot newBase;
            // Both the base and derived location moved by the same stack offset.
            StorePlain(newBase, to_zaddress(raw(oldBase) + stackOffset));
            RebaseDerived(newDerivedSlot, newBase, offset);
        }

        uwContext.anchorFA = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(uwContext.anchorFA) + stackOffset);

        // stackwm #7: publish movable-stack generation. cursorIndex is logical — not rebased.
        stackWatermark.OnStackGrow(stackOffset);
        MutatorUnlock();

        return stackOffset;
    }
    return 0;
}

// Headered Cangjie stack object (TypeInfo in the first word) vs headerless
// by-value ABI record (String = {i8*, i32, i32}). struct-live slots hold the
// latter as a pointer-to-record; HotSpot oop maps name the *location of an
// oop* (zMark.cpp:691 ZUncoloredRoot::mark reads the slot).
static bool IsHeaderedStackObject(BaseObject* obj)
{
    if (obj == nullptr || !obj->IsValidObject()) {
        return false;
    }
    TypeInfo* tip = obj->GetTypeInfo();
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    // zVerify.cpp:169 / zHeapIterator.cpp:145 consume oop slots. The Cangjie
    // ABI also supplies headerless records whose first word is a heap oop,
    // never a TypeInfo. Classify that form before interpreting its payload.
    if (tip == nullptr || tipAddr < 4096 || (tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0 ||
        Heap::IsHeapAddress(tipAddr)) {
        return false;
    }
    return tip->IsVaildType();
}

// zVerify.cpp:333 / zHeapIterator.cpp:145 consume heap-oop slots. Cangjie
// stack maps can instead name a stack object or a headerless ABI record.
// Expand those containers without healing roots or publishing marking work.
void Mutator::VisitHeapRootSlots(ObjectRef& root, const RootVisitor& visitor)
{
    std::set<BaseObject*> seen;
    std::vector<ObjectRef*> pending { &root };
    while (!pending.empty()) {
        ObjectRef& slot = *pending.back();
        pending.pop_back();
        const uintptr_t address = raw(slot.LoadPlain(std::memory_order_acquire));
        if (!IsStackAddr(address)) {
            // Preserve invalid non-stack addresses for the verifier to reject.
            visitor(slot);
            continue;
        }
        auto* object = reinterpret_cast<BaseObject*>(address);
        if (!seen.insert(object).second) { continue; }
        if (IsHeaderedStackObject(object)) {
            object->ForEachRefField([&](RefField<>& field) {
                pending.push_back(&RootSlotAt(static_cast<void*>(&field)));
            });
        } else {
            // struct-live argument form: the reference is the first record word.
            pending.push_back(&RootSlotAt(static_cast<void*>(object)));
        }
    }
}

inline void CheckAndPush(BaseObject* obj, std::set<BaseObject*>& rootSet, std::stack<BaseObject*>& rootStack)
{
    if (!IsHeaderedStackObject(obj)) {
        return;
    }
    if (!rootSet.insert(obj).second) {
        return;
    }

    if (obj->HasRefField()) {
        rootStack.push(obj);
    }
}

// Stack-map and FFI root carriers contain plain addresses and remain committed
// until this root pass completes; no colored-value decode applies here.
static BaseObject* PlainRootObject(zaddress_unsafe address)
{
    return to_object(safe(address));
}



// Eager ZUncoloredRoot::barrier (zUncoloredRoot.inline.hpp:38-59).
// The handshake owns the actual ABI slot. Keep its observed color through
// resolution, publish the current object, and only then restore a plain word.
// The eager scan captures the saved thread color before the watermark installs
// new masks; every root in that scan must keep using the captured color.
static bool PushHeapRoot(RootSlot& root, bool young, uintptr_t color, bool follow = true)
{
    (void)young;
    const zaddress_unsafe observed = root.LoadPlain();
    BaseObject* object = PlainRootObject(observed);
    if (!Heap::IsHeapAddress(object)) {
        return false;
    }
    zaddress_unsafe* slot = reinterpret_cast<zaddress_unsafe*>(&root);
    if (follow) {
        ZUncoloredRoot::mark(slot, color);
    } else {
        ZUncoloredRoot::process_invisible(slot, color);
    }
    return Heap::IsHeapAddress(PlainRootObject(root.LoadPlain()));
}

static bool PushHeaderlessRecordField(BaseObject* record, const char* site, bool young, uintptr_t color)
{
    if (record == nullptr) {
        return false;
    }
    // This is record+0 itself, not a copy of the field or its decoded value.
    RootSlot& field = RootSlotAt(static_cast<void*>(record));
    return PushHeapRoot(field, young, color);
}

bool Mutator::GcPhaseEnum(bool young, uint64_t stackScanEpoch, bool bySelf, size_t* scannedFrames)
{
    // ZGC zStackWatermark.cpp:163-214: the closure reads the color saved in
    // start_processing_impl, not the previous epoch's headColor.
    RootVisitor visitor = [this, young, stackScanEpoch](ObjectRef& root) {
        const uintptr_t rootColor = stackScanEpoch == 0 ? GetGCData().storeGoodMask
            : GetStackWatermark().uncolored_root_color();
        VisitHeapRootSlots(root, [young, rootColor](ObjectRef& slot) {
            (void)PushHeapRoot(slot, young, rootColor);
        });
    };
    RootVisitor invisibleRootVisitor = [this, young, stackScanEpoch](ObjectRef& root) {
        const uintptr_t rootColor = stackScanEpoch == 0 ? GetGCData().storeGoodMask
            : GetStackWatermark().uncolored_root_color();
        (void)PushHeapRoot(root, young, rootColor, false);
    };
    DerivedPtrVisitor derivedVisitor = MakeDerivedRootVisitor(visitor);
    if (stackScanEpoch == 0) {
        VisitHeapReferences(visitor, visitor, derivedVisitor, visitor, invisibleRootVisitor, young);
        return true;
    }
    size_t frames = 0;
    (void)bySelf;
    bool scanned = StackWatermarkSet::finish_processing(*this, visitor, invisibleRootVisitor, stackScanEpoch,
                                                        &derivedVisitor, frames);
    if (scannedFrames != nullptr) {
        *scannedFrames = frames;
    }
    return scanned;
}

DerivedPtrVisitor Mutator::MakeDerivedRootVisitor(const RootVisitor& visitor)
{
    // oopMap.cpp:400-421, ProcessDerivedOop: retain the old offset and apply
    // the same ordinary-root closure to a copy of the base in the derived slot.
    return [visitor](BasePtrType base, DerivedSlot& derived) {
        if (is_null(base) || is_null(derived.LoadDerived())) {
            return;
        }
        const uintptr_t offset = raw(derived.LoadDerived()) - raw(base);
        RootSlot baseValue;
        StorePlain(baseValue, safe(base));
        RebaseDerived(derived, baseValue, 0);
        // ProcessDerivedOop temporarily treats this very derived word as an oop.
        // Preserve its slot identity for closures that deduplicate physical roots.
        RootSlot& currentBase = RootSlotAt(static_cast<void*>(&derived));
        visitor(currentBase);
        RebaseDerived(derived, currentBase, offset);
    };
}

inline void Mutator::HandleCpuProfile()
{
    MutatorLock();
    // the stack doesn't include managed frame, skip it.
    if (!IsManagedContext()) {
        MutatorUnlock();
        return;
    }
    IncObserver();
    StackManager::PrintStackTraceForCpuProfile(&(GetUnwindContext()), GetCJThreadId());
    DecObserver();
    MutatorUnlock();
}

void Mutator::WaitForCpuProfiling() const
{
    while (GetCpuProfileState() != FINISH_CPUPROFILE || CpuProfileRequestQueued(this)) {
        (void)sched_yield();
    }
}

bool Mutator::TransitionToCpuProfile(bool bySelf)
{
    for (;;) {
        CpuProfileState state = GetCpuProfileState();
        if (state == FINISH_CPUPROFILE && !CpuProfileRequestQueued(this)) {
            return true;
        }
        if (state == IN_CPUPROFILING) {
            if (bySelf) {
                WaitForCpuProfiling();
                return true;
            }
            return false;
        }
        if (!bySelf && state == NO_CPUPROFILE && !CpuProfileRequestQueued(this)) {
            return true;
        }
        if (!ClaimCpuProfileRequest(this)) {
            continue;
        }
        TransitionToCpuProfileExclusive();
        CompleteCpuProfileRequest(this);
        return true;
    }
}

void Mutator::TransitionToCpuProfileExclusive()
{
    HandleCpuProfile();
}

void Mutator::ReleaseAllocBuffer()
{
    AllocBuffer* buffer = GetAllocBuffer();
    buffer->Fini();
    if (ThreadLocal::GetAllocBuffer() == buffer) { ThreadLocal::SetAllocBuffer(nullptr); }
    // We can remove foreign thread c-heap resource here.
}
} // namespace MapleRuntime
