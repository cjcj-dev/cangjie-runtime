// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Common/X86StubLayout.h"

#define cfi_rel_offset(reg, off)	.cfi_rel_offset reg, off
#define cfi_restore(reg)		.cfi_restore reg
#define cfi_def_cfa_register(reg)	.cfi_def_cfa_register reg

#define StackGrowStubSize           (0x264)

// This signal handler stub relies on MRT_PrepareToHandleSignal to put continuation pc in x17.
// Continuation pc describes the control flow after signal handling completes.

    .text
    .align 2
    .global _CJ_MCC_StackGrowStub
_CJ_MCC_StackGrowStub:
    .cfi_startproc
    pushq   %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq    %rsp, %rbp
    .cfi_def_cfa_register %rbp
    .set mrt_stub_depth, 0
    MRT_X86_STUB_GPRS(MRT_X86_STUB_PUSH)
    .if mrt_stub_depth != MRT_X86_STUB_GPR_BYTES
    .error "stub GPR area size mismatch"
    .endif

    // Need to ensure 16-bit alignment
    subq   $MRT_X86_STUB_VECTOR_BYTES, %rsp

    MRT_X86_STUB_XMMS(MRT_X86_STUB_SAVE_XMM)

    movq   %rbp,     %rsi
    movq   $1,       %rdx
    movq   %r15,     %rcx
    leaq   21(%rip), %rdi // rdi is addr of unwindPCForStackGrowStub
    callq  _MRT_UpdateUwContext

    // The input rbp after stack expansion is used to determine 
    // whether the current stack expansion meets requirements.
    movq   %rbp,     %rdi
    movq   -8(%rbp), %rsi
    movq   8(%rbp),  %rdx
    callq  _MRT_StackGrow
L_unwindPCForStackGrowStub:

    addq %rax, %rsp
    addq %rax, %rbp

    movq   %rax, %rdi
    callq  _MRT_FreeOldStack

    callq  _MRT_GetThreadLocalData
    movq   %rax, %r15
    movq   %rax, %rdi
    callq  _MRT_DeleteC2NContext

    // restore callee registers
    MRT_X86_STUB_XMMS(MRT_X86_STUB_RESTORE_XMM)

    // Need to ensure 16-bit alignment
    addq  $MRT_X86_STUB_VECTOR_BYTES, %rsp

    popq  %rax
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %r11
    popq  %r10
    popq  %r9
    popq  %r8
    popq  %rsi
    popq  %rsi
    popq  %rdi
    popq  %rdx
    popq  %rcx
    popq  %rbx
    popq  %rax
    popq  %rbp
    cfi_restore (%rax)
    cfi_restore (%r14)
    cfi_restore (%r13)
    cfi_restore (%r12)
    cfi_restore (%r11)
    cfi_restore (%r10)
    cfi_restore (%r9)
    cfi_restore (%r8)
    cfi_restore (%rsi)
    cfi_restore (%rsi)
    cfi_restore (%rdi)
    cfi_restore (%rdx)
    cfi_restore (%rcx)
    cfi_restore (%rbx)
    cfi_restore (%rax)
    cfi_restore (%rbp)
    .cfi_def_cfa %rsp, 8

    retq

    .cfi_endproc

    .globl _unwindPCForStackGrowStub
_unwindPCForStackGrowStub = L_unwindPCForStackGrowStub
