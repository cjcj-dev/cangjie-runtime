// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// stub need to be 16-byte aligned, so we should use (8 * even numbers)
#define CalleeSavedRegistersStubFrameSize           (8 * 8)

////////////////////////////////////////////////////////////////////////////////
// CalleeSavedRegistersStub simply forwards arguments passed by runtime, i.e., arguments for compiled method are passed
// according to C/C++ calling convention, which usually means efficiency.
////////////////////////////////////////////////////////////////////////////////
// the frame layout of stack(growing downwards) after CalleeSavedRegistersStub frame is built looks like:
//                 |  ...         |
//                 |  ra          | return address for the caller of CalleeSavedRegistersStub
// caller rbp  --> |  rbp         |
//                 |  ...         |
//                 | ra           |
//   stub rbp  --> | caller rbp   | <==CalleeSavedRegistersStub Start
// callee register | %r15         |
//                 | %r14         |
//                 | %r13         |
//                 | %r12         |
//                 | %rbx         |
// arg0            | %rdi         |
// arg1            | %rsi         |
//                 | null         |
// 16-byte aligned | null         | <==CalleeSavedRegistersStubFrameSize, CalleeSavedRegistersStub frame ends at here

    .macro CalleeSavedRegistersStub, funcName, funcNameStub
    .text
    .align 2
    .global \funcNameStub
    .type \funcNameStub, @function
\funcNameStub:
    .cfi_startproc
    pushq   %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq    %rsp, %rbp
    .cfi_def_cfa_register %rbp
    subq    $CalleeSavedRegistersStubFrameSize, %rsp

    movq    %r15, -8(%rbp)
    movq    %r14, -16(%rbp)
    movq    %r13, -24(%rbp)
    movq    %r12, -32(%rbp)
    movq    %rbx, -40(%rbp)
    .cfi_rel_offset %rbx, -40
    .cfi_rel_offset %r12, -32
    .cfi_rel_offset %r13, -24
    .cfi_rel_offset %r14, -16
    .cfi_rel_offset %r15, -8

    callq   \funcName
    movq    %rax, -8(%rbp)

    callq   MRT_GetThreadLocalData@PLT
    movq    %rax,  %r15

    // Restore the value of rsp before coping arg6, arg7...
    addq    $CalleeSavedRegistersStubFrameSize, %rsp

    movq    -8(%rbp),  %rax
    movq    -16(%rbp), %r14
    movq    -24(%rbp), %r13
    movq    -32(%rbp), %r12
    movq    -40(%rbp), %rbx

    popq    %rbp
    .cfi_def_cfa %rsp, 8
    retq
    .cfi_endproc
    .size \funcNameStub, .-\funcNameStub
    .endm

////////////////////////////////////////////////////////////////////////////////
// ApplyCJRegistersStub is CalleeSavedRegistersStub for the four CJ_MCC_ApplyCJ* entries:
// same callee-saved slots, same C2R classification. Their value records may not fit the
// stack left to the C++ entry; ApplyCJMethod then returns a growth request rather than
// raising StackOverflowError (MRT_TakeEntryStackGrowRequest). The growth runs here, after
// the current C++ entry returned; outer native frames may still be live. Linux
// fixed-address capacity growth preserves them. The published context is this C2R frame
// (its pc lies in the C2R range) and FixExtendedStack walks it and the managed callers as
// for a managed-callee growth. The argument slots below are the only other native-held
// words; MRT_EntryStackGrow moves them. The entry then runs once more, unarmed, so its
// value-record guard stays the final check.
//   stub rbp - 8 .. - 40 | r15 r14 r13 r12 rbx (CalleeSavedRegistersStub layout)
//   stub rbp - 48        | rcx  argument slots, argSlots = rbp - 72: rdi rsi rdx rcx
//   stub rbp - 56        | rdx
//   stub rbp - 64        | rsi
//   stub rbp - 72        | rdi
//   stub rbp - 80        | entry result
//   stub rbp - 88        | requested lowest address
////////////////////////////////////////////////////////////////////////////////
#define ApplyCJRegistersStubFrameSize           (8 * 12)

    .macro ApplyCJRegistersStub, funcName, funcNameStub, hasReceiver
    .text
    .align 2
    .global \funcNameStub
    .type \funcNameStub, @function
\funcNameStub:
    .cfi_startproc
    pushq   %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq    %rsp, %rbp
    .cfi_def_cfa_register %rbp
    subq    $ApplyCJRegistersStubFrameSize, %rsp

    movq    %r15, -8(%rbp)
    movq    %r14, -16(%rbp)
    movq    %r13, -24(%rbp)
    movq    %r12, -32(%rbp)
    movq    %rbx, -40(%rbp)
    .cfi_rel_offset %rbx, -40
    .cfi_rel_offset %r12, -32
    .cfi_rel_offset %r13, -24
    .cfi_rel_offset %r14, -16
    .cfi_rel_offset %r15, -8

    movq    %rdi, -72(%rbp)
    movq    %rsi, -64(%rbp)
    movq    %rdx, -56(%rbp)
    movq    %rcx, -48(%rbp)
    callq   MRT_ArmEntryStackGrow@PLT
1:
    movq    -72(%rbp), %rdi
    movq    -64(%rbp), %rsi
    movq    -56(%rbp), %rdx
    movq    -48(%rbp), %rcx
    callq   \funcName
    movq    %rax, -80(%rbp)

    callq   MRT_TakeEntryStackGrowRequest@PLT
    testq   %rax, %rax
    jz      2f
    movq    %rax, -88(%rbp)

    callq   MRT_GetThreadLocalData@PLT
    movq    %rax, %rcx
    movq    %rbp, %rsi
    movq    $1, %rdx
    leaq    3f(%rip), %rdi
    callq   MRT_UpdateUwContext@PLT

    movq    %rbp, %rdi
    movq    -88(%rbp), %rsi
    leaq    -72(%rbp), %rdx
    movq    $\hasReceiver, %rcx
    callq   MRT_EntryStackGrow@PLT
3:
    addq    %rax, %rsp
    addq    %rax, %rbp

    movq    %rax, %rdi
    callq   MRT_FreeOldStack@PLT

    callq   MRT_GetThreadLocalData@PLT
    movq    %rax, %rdi
    callq   MRT_DeleteC2NContext@PLT
    jmp     1b
2:
    callq   MRT_GetThreadLocalData@PLT
    movq    %rax,  %r15

    addq    $ApplyCJRegistersStubFrameSize, %rsp

    movq    -80(%rbp), %rax
    movq    -16(%rbp), %r14
    movq    -24(%rbp), %r13
    movq    -32(%rbp), %r12
    movq    -40(%rbp), %rbx

    popq    %rbp
    .cfi_def_cfa %rsp, 8
    retq
    .cfi_endproc
    .size \funcNameStub, .-\funcNameStub
    .endm

    .macro LocalRegionStub, funcName, funcNameStub
    .text
    .align 2
    .global \funcNameStub
    .type \funcNameStub, @function
\funcNameStub:
    .cfi_startproc
    pushq   %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq    %rsp, %rbp
    .cfi_def_cfa_register %rbp
    subq    $CalleeSavedRegistersStubFrameSize, %rsp

    movq    %r15, -8(%rbp)
    movq    %r14, -16(%rbp)
    movq    %r13, -24(%rbp)
    movq    %r12, -32(%rbp)
    movq    %rbx, -40(%rbp)
    .cfi_rel_offset %rbx, -40
    .cfi_rel_offset %r12, -32
    .cfi_rel_offset %r13, -24
    .cfi_rel_offset %r14, -16
    .cfi_rel_offset %r15, -8

    movq    0(%rbp), %rdi
    callq   \funcName

    callq   MRT_GetThreadLocalData@PLT
    movq    %rax,  %r15

    addq    $CalleeSavedRegistersStubFrameSize, %rsp

    movq    -16(%rbp), %r14
    movq    -24(%rbp), %r13
    movq    -32(%rbp), %r12
    movq    -40(%rbp), %rbx

    popq    %rbp
    .cfi_def_cfa %rsp, 8
    retq
    .cfi_endproc
    .size \funcNameStub, .-\funcNameStub
    .endm

.global unwindPCForC2RStubStart
unwindPCForC2RStubStart:
    CalleeSavedRegistersStub MCC_PackageInitBegin CJ_MCC_PackageInitBegin
    CalleeSavedRegistersStub MCC_PackageInitComplete CJ_MCC_PackageInitComplete
    CalleeSavedRegistersStub MCC_PackageInitFail CJ_MCC_PackageInitFail
    CalleeSavedRegistersStub MCC_PackageInitAbort CJ_MCC_PackageInitAbort
CalleeSavedRegistersStub MCC_NewObject CJ_MCC_NewObject
CalleeSavedRegistersStub MCC_NewWeakRefObject CJ_MCC_NewWeakRefObject
CalleeSavedRegistersStub MCC_NewObjArray CJ_MCC_NewObjArray
CalleeSavedRegistersStub MCC_NewArray CJ_MCC_NewArray
CalleeSavedRegistersStub MCC_NewArray8 CJ_MCC_NewArray8
CalleeSavedRegistersStub MCC_NewArray16 CJ_MCC_NewArray16
CalleeSavedRegistersStub MCC_NewArray32 CJ_MCC_NewArray32
CalleeSavedRegistersStub MCC_NewArray64 CJ_MCC_NewArray64
CalleeSavedRegistersStub MCC_InvokeGCImpl CJ_MCC_InvokeGC
CalleeSavedRegistersStub MCC_ReferenceInvokeFinalize CJ_MCC_ReferenceInvokeFinalize
CalleeSavedRegistersStub MCC_OnFinalizerCreated CJ_MCC_OnFinalizerCreated
CalleeSavedRegistersStub MCC_FillInStackTraceImpl CJ_MCC_FillInStackTrace
CalleeSavedRegistersStub MCC_StringDedupCanonicalImpl CJ_MCC_StringDedupCanonical
CalleeSavedRegistersStub MCC_DecodeStackTraceImpl CJ_MCC_DecodeStackTrace
CalleeSavedRegistersStub MCC_GetAllThreadSnapshotImpl CJ_MCC_GetAllThreadSnapshot
CalleeSavedRegistersStub MCC_GetCurrentThreadSnapshotImpl CJ_MCC_GetCurrentThreadSnapshot
CalleeSavedRegistersStub MCC_AcquireRawData CJ_MCC_AcquireRawData
CalleeSavedRegistersStub MRT_FutureWait CJ_MCC_FutureWait
CalleeSavedRegistersStub MCC_MutexLock CJ_MCC_MutexLock
CalleeSavedRegistersStub MCC_MutexLockSlowPath CJ_MCC_MutexLockSlowPath
CalleeSavedRegistersStub MCC_MonitorWait CJ_MCC_MonitorWait
CalleeSavedRegistersStub MCC_MultiConditionMonitorWait CJ_MCC_MultiConditionMonitorWait
CalleeSavedRegistersStub MRT_Sleep CJ_MRT_Sleep
CalleeSavedRegistersStub MRT_ThreadWait CJ_MRT_ThreadWait
CalleeSavedRegistersStub MRT_ThreadResumeAndWait CJ_MRT_ThreadResumeAndWait
CalleeSavedRegistersStub MRT_ThreadReady CJ_MRT_ThreadReady
CalleeSavedRegistersStub MCC_NewPinnedObject CJ_MCC_NewPinnedObject
CalleeSavedRegistersStub MCC_NewCJThread CJ_MCC_NewCJThread
CalleeSavedRegistersStub MCC_NewCJThreadNoReturn CJ_MCC_NewCJThreadNoReturn
ApplyCJRegistersStub MCC_ApplyCJInstanceMethod CJ_MCC_ApplyCJInstanceMethod 1
ApplyCJRegistersStub MCC_ApplyCJStaticMethod CJ_MCC_ApplyCJStaticMethod 0
ApplyCJRegistersStub MCC_ApplyCJGenericInstanceMethod CJ_MCC_ApplyCJGenericInstanceMethod 1
ApplyCJRegistersStub MCC_ApplyCJGenericStaticMethod CJ_MCC_ApplyCJGenericStaticMethod 0
CalleeSavedRegistersStub MCC_GetMethodAnnotations CJ_MCC_GetMethodAnnotations
CalleeSavedRegistersStub MCC_GetInstanceFieldAnnotations CJ_MCC_GetInstanceFieldAnnotations
CalleeSavedRegistersStub MCC_GetStaticFieldAnnotations CJ_MCC_GetStaticFieldAnnotations
CalleeSavedRegistersStub MCC_GetParameterAnnotations CJ_MCC_GetParameterAnnotations
CalleeSavedRegistersStub MCC_GetTypeInfoAnnotations CJ_MCC_GetTypeInfoAnnotations
CalleeSavedRegistersStub MCC_GetInstanceFieldValue CJ_MCC_GetInstanceFieldValue
CalleeSavedRegistersStub MCC_GetStaticFieldValue CJ_MCC_GetStaticFieldValue
CalleeSavedRegistersStub MCC_NewArrayGeneric CJ_MCC_NewArrayGeneric
CalleeSavedRegistersStub MCC_DumpCJHeapData CJ_MCC_DumpCJHeapData
CalleeSavedRegistersStub MCC_NewAndInitEnumTupleObject CJ_MCC_NewAndInitEnumTupleObject
CalleeSavedRegistersStub MCC_GetAssociatedValues CJ_MCC_GetAssociatedValues
    LocalRegionStub MCC_StartLocalRegion CJ_MCC_StartLocalRegion
    LocalRegionStub MCC_EndLocalRegion CJ_MCC_EndLocalRegion
    CalleeSavedRegistersStub MCC_NewLocalObject CJ_MCC_NewLocalObject
    CalleeSavedRegistersStub MCC_NewLocalFinalizer CJ_MCC_NewLocalFinalizer
    CalleeSavedRegistersStub MCC_NewLocalArray CJ_MCC_NewLocalArray
    CalleeSavedRegistersStub MCC_NewLocalObjArray CJ_MCC_NewLocalObjArray
    CalleeSavedRegistersStub MCC_NewLocalArray8 CJ_MCC_NewLocalArray8
    CalleeSavedRegistersStub MCC_NewLocalArray16 CJ_MCC_NewLocalArray16
    CalleeSavedRegistersStub MCC_NewLocalArray32 CJ_MCC_NewLocalArray32
    CalleeSavedRegistersStub MCC_NewLocalArray64 CJ_MCC_NewLocalArray64
    CalleeSavedRegistersStub MCC_NewLocalGenericObject CJ_MCC_NewLocalGenericObject
    CalleeSavedRegistersStub MCC_NewLocalGenericArray CJ_MCC_NewLocalGenericArray
    CalleeSavedRegistersStub MCC_NewLocalWeakRefObject CJ_MCC_NewLocalWeakRefObject
    CalleeSavedRegistersStub MCC_AddLocalFinalizer CJ_MCC_AddLocalFinalizer
    CalleeSavedRegistersStub MCC_RemoveLocalFinalizer CJ_MCC_RemoveLocalFinalizer

.global unwindPCForC2RStubEnd
unwindPCForC2RStubEnd:
