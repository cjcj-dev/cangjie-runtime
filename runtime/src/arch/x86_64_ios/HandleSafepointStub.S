// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// stub need to be 16-byte aligned, so we should use (8 * even numbers)
#include "Common/X86StubLayout.h"

#define HandleSafepointStubFrameSize           MRT_X86_STUB_VECTOR_BYTES

#define cfi_adjust_cfa_offset(off)	.cfi_adjust_cfa_offset off
#define cfi_rel_offset(reg, off)	.cfi_rel_offset reg, off
#define cfi_restore(reg)		.cfi_restore reg
#define cfi_def_cfa_register(reg)	.cfi_def_cfa_register reg

    .text
    .align 2
    .global _CJ_MCC_HandleSafepoint
_CJ_MCC_HandleSafepoint:
    .cfi_startproc

    // save callee registers
    pushq  %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq   %rsp, %rbp
    .cfi_def_cfa_register %rbp

    .set mrt_stub_depth, 0
    MRT_X86_STUB_GPRS(MRT_X86_STUB_PUSH)
    .if mrt_stub_depth != MRT_X86_STUB_GPR_BYTES
    .error "stub GPR area size mismatch"
    .endif

    // Need to ensure 16-bit alignment
    subq   $HandleSafepointStubFrameSize, %rsp

    MRT_X86_STUB_XMMS(MRT_X86_STUB_SAVE_XMM)

    movq   %rbp,     %rsi
    movq   %r15,     %rcx
    movq   $3,       %rdx
    leaq   13(%rip), %rdi // rdi is addr of unwindPCForSafepointHandlerStub
    callq  _MRT_UpdateUwContext

    // call safepoint handler
    movq   %r15,     %rdi
    callq  _HandleSafepoint
L_unwindPCForSafepointHandlerStub:
    callq  _MRT_GetThreadLocalData
    movq   %rax, %r15
    movq   %rax, %rdi
    callq  _MRT_DeleteC2NContext

    // restore callee registers
    MRT_X86_STUB_XMMS(MRT_X86_STUB_RESTORE_XMM)

    // Need to ensure 16-bit alignment
    addq  $HandleSafepointStubFrameSize, %rsp

    // do not popq %r15
    popq  %rax
    cfi_restore (%rax)
    popq  %r14
    cfi_restore (%r14)
    popq  %r13
    cfi_restore (%r13)
    popq  %r12
    cfi_restore (%r12)
    popq  %r11
    cfi_restore (%r11)
    popq  %r10
    cfi_restore (%r10)
    popq  %r9
    cfi_restore (%r9)
    popq  %r8
    cfi_restore (%r8)
    popq  %rsp
    cfi_restore (%rsp)
    popq  %rsi
    cfi_restore (%rsi)
    popq  %rdi
    cfi_restore (%rdi)
    popq  %rdx
    cfi_restore (%rdx)
    popq  %rcx
    cfi_restore (%rcx)
    popq  %rbx
    cfi_restore (%rbx)
    popq  %rax
    cfi_restore (%rax)
    popq  %rbp
    cfi_restore (%rbp)
    .cfi_def_cfa %rsp, 8

    retq
    .cfi_endproc

    .globl _unwindPCForSafepointHandlerStub
_unwindPCForSafepointHandlerStub = L_unwindPCForSafepointHandlerStub
