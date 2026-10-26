// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

    .def    CJ_MCC_ThrowStackOverflowError;
    .scl    2;
    .type   32;
    .endef
    .globl  CJ_MCC_ThrowStackOverflowError            # -- Begin function CJ_MCC_ThrowStackOverflowError
    .p2align    4, 0x90
CJ_MCC_ThrowStackOverflowError:                   # @CJ_MCC_ThrowStackOverflowError
.seh_proc CJ_MCC_ThrowStackOverflowError
# %bb.0:
    pushq   %rbp
    .seh_pushreg %rbp
    subq    $96, %rsp
    .seh_stackalloc 96
    leaq    96(%rsp), %rbp
    .seh_setframe %rbp, 96
    movq %r15, -8(%rbp)
    .seh_savereg %r15, 88
    movq %r14, -16(%rbp)
    .seh_savereg %r14, 80
    movq %r13, -24(%rbp)
    .seh_savereg %r13, 72
    movq %r12, -32(%rbp)
    .seh_savereg %r12, 64
    movq %rdi, -40(%rbp)
    .seh_savereg %rdi, 56
    movq %rsi, -48(%rbp)
    .seh_savereg %rsi, 48
    movq %rbx, -56(%rbp)
    .seh_savereg %rbx, 40
    .seh_endprologue
    .global windowsStackOverflowBodyStart
windowsStackOverflowBodyStart:
    callq  MCC_ThrowStackOverflowError
    movq   %rbp, %rcx
    xorl   %edx, %edx
    callq  MRT_CompleteWindowsExceptionReturn
    .global windowsStackOverflowBodyEnd
windowsStackOverflowBodyEnd:
    movq -8(%rbp), %r15
    movq -16(%rbp), %r14
    movq -24(%rbp), %r13
    movq -32(%rbp), %r12
    movq -40(%rbp), %rdi
    movq -48(%rbp), %rsi
    movq -56(%rbp), %rbx
    leaq    0(%rbp), %rsp
    popq    %rbp
    retq
    .seh_endproc
