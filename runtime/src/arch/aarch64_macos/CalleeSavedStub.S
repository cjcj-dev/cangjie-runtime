// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Common/Aarch64StubLayout.h"
MRT_AARCH64_STUB_CALLEE_GPRS(MRT_AARCH64_CHECK_CALLEE_SLOT)

#define cfi_adjust_cfa_offset(off)      .cfi_adjust_cfa_offset off
#define cfi_rel_offset(reg, off)        .cfi_rel_offset reg, off
#define cfi_restore(reg)                .cfi_restore reg
#define cfi_def_cfa_register(reg)       .cfi_def_cfa_register reg

////////////////////////////////////////////////////////////////////////////////
// CalleeSavedRegistersStub simply forwards arguments passed by runtime, i.e., arguments for compiled method are passed
// according to C/C++ calling convention, which usually means efficiency.
////////////////////////////////////////////////////////////////////////////////

#define StubFrameContextSize      (8 * 14)
#define StubCalleeSaveAreaSize MRT_AARCH64_STUB_CALLEE_BASE

// the frame layout of stack(growing downwards) after MCC_C2NStub frame is built looks like:

// caller sp  -->  | arg7         |
// callee saved    | avalid null  |
//                 | x28          |
//                 | x27          |
//                 | x26          |
//                 | x25          |
//                 | x24          |
//                 | x23          |
//                 | x22          |
//                 | x21          |
//                 | x20          |
// callee saved    | x19          |
//                 | x30          |
//   stub fp  -->  | caller fp    |
//                 |  ...         |
//                 | arg10        |
//                 | arg9         |
//                 | arg8         |
//   stub sp  -->  | arg7         | <== MCC_C2NStub frame ends at here

.macro CalleeSavedRegistersStub, funcName
    .cfi_startproc
#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    paciasp
    .cfi_negate_ra_state
#endif
    stp  x29, x30, [sp,  #-StubFrameContextSize]!
    cfi_adjust_cfa_offset (StubFrameContextSize)
    cfi_rel_offset (x29, 0)
    cfi_rel_offset (x30, 8)

    mov  x29, sp
    cfi_def_cfa_register (sp)

    // save all used callee-saved registers.
    stp  x19, x20, [sp, #.Lmrt_a64_callee_x19]
    cfi_rel_offset (x19, .Lmrt_a64_callee_x19)
    cfi_rel_offset (x20, .Lmrt_a64_callee_x20)

    stp  x21, x22, [sp, #.Lmrt_a64_callee_x21]
    cfi_rel_offset (x21, .Lmrt_a64_callee_x21)
    cfi_rel_offset (x22, .Lmrt_a64_callee_x22)

    stp  x23, x24, [sp, #.Lmrt_a64_callee_x23]
    cfi_rel_offset (x23, .Lmrt_a64_callee_x23)
    cfi_rel_offset (x24, .Lmrt_a64_callee_x24)

    stp  x25, x26, [sp, #.Lmrt_a64_callee_x25]
    cfi_rel_offset (x25, .Lmrt_a64_callee_x25)
    cfi_rel_offset (x26, .Lmrt_a64_callee_x26)

    stp  x27, x28, [sp, #.Lmrt_a64_callee_x27]
    cfi_rel_offset (x27, .Lmrt_a64_callee_x27)
    cfi_rel_offset (x28, .Lmrt_a64_callee_x28)

    bl   _\funcName
    str  x0,  [sp, #.Lmrt_a64_callee_x28]

    bl   _MRT_GetThreadLocalData
    mov  x28, x0

    // restore all used callee-saved registers.
    ldp  x19, x20, [sp, #.Lmrt_a64_callee_x19]
    cfi_restore (x19)
    cfi_restore (x20)
    ldp  x21, x22, [sp, #.Lmrt_a64_callee_x21]
    cfi_restore (x21)
    cfi_restore (x22)
    ldp  x23, x24, [sp, #.Lmrt_a64_callee_x23]
    cfi_restore (x23)
    cfi_restore (x24)
    ldp  x25, x26, [sp, #.Lmrt_a64_callee_x25]
    cfi_restore (x25)
    cfi_restore (x26)
    ldr  x27, [sp, #.Lmrt_a64_callee_x27]
    cfi_restore (x27)
    ldr  x0,  [sp, #.Lmrt_a64_callee_x28]
    cfi_restore (x0)

    ldp  x29, x30, [sp], #StubFrameContextSize
    cfi_adjust_cfa_offset (-StubFrameContextSize)
    cfi_restore (x29)
    cfi_restore (x30)
#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    // ExceptionHandling::ChangeEHStackInfoLR signs with FA + frame size.
    autiasp
    .cfi_negate_ra_state
#endif
    ret
    .cfi_endproc
.endm

    .global _unwindPCForC2RStubStart
_unwindPCForC2RStubStart:
    .text
    .align 2
    .global _CJ_MCC_PackageInitBegin
_CJ_MCC_PackageInitBegin:
    CalleeSavedRegistersStub MCC_PackageInitBegin

    .text
    .align 2
    .global _CJ_MCC_PackageInitComplete
_CJ_MCC_PackageInitComplete:
    CalleeSavedRegistersStub MCC_PackageInitComplete

    .text
    .align 2
    .global _CJ_MCC_PackageInitFail
_CJ_MCC_PackageInitFail:
    CalleeSavedRegistersStub MCC_PackageInitFail

    .text
    .align 2
    .global _CJ_MCC_PackageInitAbort
_CJ_MCC_PackageInitAbort:
    CalleeSavedRegistersStub MCC_PackageInitAbort


    .text
    .align 2
    .global _CJ_MCC_NewObject
_CJ_MCC_NewObject:
    // B/BL callers pass the current FP/LR; this stub owns its frame.
    CalleeSavedRegistersStub MCC_NewObject

    .text
    .align 2
    .global _CJ_MCC_NewWeakRefObject
_CJ_MCC_NewWeakRefObject:
    CalleeSavedRegistersStub MCC_NewWeakRefObject

    .text
    .align 2
    .global _CJ_MCC_NewFinalizer
_CJ_MCC_NewFinalizer:
    CalleeSavedRegistersStub MCC_NewFinalizer

    .text
    .align 2
    .global _CJ_MCC_NewObjArray
_CJ_MCC_NewObjArray:
    CalleeSavedRegistersStub MCC_NewObjArray

    .text
    .align 2
    .global _CJ_MCC_NewArray
_CJ_MCC_NewArray:
    CalleeSavedRegistersStub MCC_NewArray

    .text
    .align 2
    .global _CJ_MCC_NewArray8
_CJ_MCC_NewArray8:
    CalleeSavedRegistersStub MCC_NewArray8

    .text
    .align 2
    .global _CJ_MCC_NewArray16
_CJ_MCC_NewArray16:
    CalleeSavedRegistersStub MCC_NewArray16

    .text
    .align 2
    .global _CJ_MCC_NewArray32
_CJ_MCC_NewArray32:
    CalleeSavedRegistersStub MCC_NewArray32

    .text
    .align 2
    .global _CJ_MCC_NewArray64
_CJ_MCC_NewArray64:
    CalleeSavedRegistersStub MCC_NewArray64

    .text
    .align 2
    .global _CJ_MCC_InvokeGC
_CJ_MCC_InvokeGC:
    CalleeSavedRegistersStub MCC_InvokeGCImpl

    .text
    .align 2
    .global _CJ_MCC_FillInStackTrace
_CJ_MCC_FillInStackTrace:
    CalleeSavedRegistersStub MCC_FillInStackTraceImpl

    .text
    .align 2
    .global _CJ_MCC_StringDedupCanonical
_CJ_MCC_StringDedupCanonical:
    CalleeSavedRegistersStub MCC_StringDedupCanonicalImpl

    .text
    .align 2
    .global _CJ_MCC_DecodeStackTrace
_CJ_MCC_DecodeStackTrace:
    CalleeSavedRegistersStub MCC_DecodeStackTraceImpl

    .text
    .align 2
    .global _CJ_MCC_GetAllThreadSnapshot
_CJ_MCC_GetAllThreadSnapshot:
    CalleeSavedRegistersStub MCC_GetAllThreadSnapshotImpl

    .text
    .align 2
    .global _CJ_MCC_GetCurrentThreadSnapshot
_CJ_MCC_GetCurrentThreadSnapshot:
    CalleeSavedRegistersStub MCC_GetCurrentThreadSnapshotImpl

    .text
    .align 2
    .global _CJ_SchedulePreemptCheckStub
_CJ_SchedulePreemptCheckStub:
    CalleeSavedRegistersStub CJ_SchedulePreemptCheck

    .text
    .align 2
    .global _CJ_MCC_AcquireRawData
_CJ_MCC_AcquireRawData:
    CalleeSavedRegistersStub MCC_AcquireRawData

    .text
    .align 2
    .global _CJ_MCC_FutureWait
_CJ_MCC_FutureWait:
    CalleeSavedRegistersStub MRT_FutureWait

    .text
    .align 2
    .global _CJ_MCC_MutexLock
_CJ_MCC_MutexLock:
    CalleeSavedRegistersStub MCC_MutexLock

    .text
    .align 2
    .global _CJ_MCC_MutexLockSlowPath
_CJ_MCC_MutexLockSlowPath:
    CalleeSavedRegistersStub MCC_MutexLockSlowPath

    .text
    .align 2
    .global _CJ_MCC_MonitorWait
_CJ_MCC_MonitorWait:
    CalleeSavedRegistersStub MCC_MonitorWait

    .text
    .align 2
    .global _CJ_MCC_MultiConditionMonitorWait
_CJ_MCC_MultiConditionMonitorWait:
    CalleeSavedRegistersStub MCC_MultiConditionMonitorWait

    .text
    .align 2
    .global _CJ_MRT_Sleep
_CJ_MRT_Sleep:
    CalleeSavedRegistersStub MRT_Sleep

    .text
    .align 2
    .global _CJ_MRT_ThreadWait
_CJ_MRT_ThreadWait:
    CalleeSavedRegistersStub MRT_ThreadWait

    .text
    .align 2
    .global _CJ_MRT_ThreadResumeAndWait
_CJ_MRT_ThreadResumeAndWait:
    CalleeSavedRegistersStub MRT_ThreadResumeAndWait

    .text
    .align 2
    .global _CJ_MRT_ThreadReady
_CJ_MRT_ThreadReady:
    CalleeSavedRegistersStub MRT_ThreadReady

    .text
    .align 2
    .global _CJ_MCC_NewPinnedObject
_CJ_MCC_NewPinnedObject:
    CalleeSavedRegistersStub MCC_NewPinnedObject

    .text
    .align 2
    .global _CJ_MCC_ApplyCJInstanceMethod
_CJ_MCC_ApplyCJInstanceMethod:
    CalleeSavedRegistersStub MCC_ApplyCJInstanceMethod

    .text
    .align 2
    .global _CJ_MCC_ApplyCJStaticMethod
_CJ_MCC_ApplyCJStaticMethod:
    CalleeSavedRegistersStub MCC_ApplyCJStaticMethod

    .text
    .align 2
    .global _CJ_MCC_ApplyCJGenericInstanceMethod
_CJ_MCC_ApplyCJGenericInstanceMethod:
    CalleeSavedRegistersStub MCC_ApplyCJGenericInstanceMethod

.text
    .align 2
    .global _CJ_MCC_ApplyCJGenericStaticMethod
_CJ_MCC_ApplyCJGenericStaticMethod:
    CalleeSavedRegistersStub MCC_ApplyCJGenericStaticMethod

    .text
    .align 2
    .global _CJ_MCC_GetMethodAnnotations
_CJ_MCC_GetMethodAnnotations:
    CalleeSavedRegistersStub MCC_GetMethodAnnotations

    .text
    .align 2
    .global _CJ_MCC_GetInstanceFieldAnnotations
_CJ_MCC_GetInstanceFieldAnnotations:
    CalleeSavedRegistersStub MCC_GetInstanceFieldAnnotations

    .text
    .align 2
    .global _CJ_MCC_GetStaticFieldAnnotations
_CJ_MCC_GetStaticFieldAnnotations:
    CalleeSavedRegistersStub MCC_GetStaticFieldAnnotations

    .text
    .align 2
    .global _CJ_MCC_GetParameterAnnotations
_CJ_MCC_GetParameterAnnotations:
    CalleeSavedRegistersStub MCC_GetParameterAnnotations

    .text
    .align 2
    .global _CJ_MCC_GetTypeInfoAnnotations
_CJ_MCC_GetTypeInfoAnnotations:
    CalleeSavedRegistersStub MCC_GetTypeInfoAnnotations

    .text
    .align 2
    .global _CJ_MCC_GetInstanceFieldValue
_CJ_MCC_GetInstanceFieldValue:
    CalleeSavedRegistersStub MCC_GetInstanceFieldValue

    .text
    .align 2
    .global _CJ_MCC_GetStaticFieldValue
_CJ_MCC_GetStaticFieldValue:
    CalleeSavedRegistersStub MCC_GetStaticFieldValue

    .text
    .align 2
    .global _CJ_MCC_NewArrayGeneric
_CJ_MCC_NewArrayGeneric:
    CalleeSavedRegistersStub MCC_NewArrayGeneric

    .text
    .align 2
    .global _CJ_MCC_DumpCJHeapData
_CJ_MCC_DumpCJHeapData:
    CalleeSavedRegistersStub MCC_DumpCJHeapData

    .text
    .align 2
    .global _CJ_MCC_NewAndInitEnumTupleObject
_CJ_MCC_NewAndInitEnumTupleObject:
    CalleeSavedRegistersStub MCC_NewAndInitEnumTupleObject

    .text
    .align 2
    .global _CJ_MCC_GetAssociatedValues
_CJ_MCC_GetAssociatedValues:
    CalleeSavedRegistersStub MCC_GetAssociatedValues

    .global _unwindPCForC2RStubEnd
_unwindPCForC2RStubEnd:

#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    .section __TEXT,__const
    .p2align 3
    .globl _CalleeSavedStubFrameSize
_CalleeSavedStubFrameSize:
    .quad StubFrameContextSize
#endif
