// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#define ReturnSafepointStubFrameSize           (8 * 46 + 32)

// The compiler tail-jumps here with r10=startPC and r11=return-site-PC.
// Keep the Windows RegRoot slots at their existing rbp offsets; reserve a
// separate 32-byte home area below them (HotSpot sharedRuntime_x86_64.cpp:273).


    .def CJ_MCC_HandleReturnSafepoint;
    .scl 2;
    .type 32;
    .endef
    .global CJ_MCC_HandleReturnSafepoint
    .p2align    4, 0x90
CJ_MCC_HandleReturnSafepoint:
    .seh_proc CJ_MCC_HandleReturnSafepoint

    pushq   %rbp
    .seh_pushreg %rbp
    movq    %rsp, %rbp
    subq    $ReturnSafepointStubFrameSize, %rsp
    .seh_stackalloc ReturnSafepointStubFrameSize

    movq    %rax, -8(%rbp)
    movq    %rbx, -16(%rbp)
    .seh_savereg %rbx, 384
    movq    %rcx, -24(%rbp)
    movq    %rdx, -32(%rbp)
    movq    %rdi, -40(%rbp)
    .seh_savereg %rdi, 360
    movq    %rsi, -48(%rbp)
    .seh_savereg %rsi, 352
    movq    %r8, -56(%rbp)
    movq    %r9, -64(%rbp)
    movq    %r10, -72(%rbp)
    movq    %r11, -80(%rbp)
    movq    %r12, -88(%rbp)
    .seh_savereg %r12, 312
    movq    %r13, -96(%rbp)
    .seh_savereg %r13, 304
    movq    %r14, -104(%rbp)
    .seh_savereg %r14, 296
    movq    %r15, -112(%rbp)
    .seh_savereg %r15, 288

    movapd  %xmm0, -128(%rbp)
    movapd  %xmm1, -144(%rbp)
    movapd  %xmm2, -160(%rbp)
    movapd  %xmm3, -176(%rbp)
    movapd  %xmm4, -192(%rbp)
    movapd  %xmm5, -208(%rbp)
    movapd  %xmm6, -224(%rbp)
    .seh_savexmm %xmm6, 176
    movapd  %xmm7, -240(%rbp)
    .seh_savexmm %xmm7, 160
    movapd  %xmm8, -256(%rbp)
    .seh_savexmm %xmm8, 144
    movapd  %xmm9, -272(%rbp)
    .seh_savexmm %xmm9, 128
    movapd  %xmm10, -288(%rbp)
    .seh_savexmm %xmm10, 112
    movapd  %xmm11, -304(%rbp)
    .seh_savexmm %xmm11, 96
    movapd  %xmm12, -320(%rbp)
    .seh_savexmm %xmm12, 80
    movapd  %xmm13, -336(%rbp)
    .seh_savexmm %xmm13, 64
    movapd  %xmm14, -352(%rbp)
    .seh_savexmm %xmm14, 48
    movapd  %xmm15, -368(%rbp)
    .seh_savexmm %xmm15, 32

    .seh_endprologue

    movq   $3, %r8
    movq   %rbp, %rdx
    leaq   unwindPCForReturnSafepointHandlerStub(%rip), %rcx
    movq   %r15, %r9
    callq  MRT_UpdateUwContext

    // call safepoint handler
    movq   %r15, %rcx
    callq  HandleReturnSafepoint
    .global unwindPCForReturnSafepointHandlerStub
unwindPCForReturnSafepointHandlerStub:
    callq  MRT_GetThreadLocalData
    movq   %rax, %r15
    movq   %rax, %rcx
    callq  MRT_DeleteC2NContext

    movq    -8(%rbp), %rax
    movq    -16(%rbp), %rbx
    movq    -24(%rbp), %rcx
    movq    -32(%rbp), %rdx
    movq    -40(%rbp), %rdi
    movq    -48(%rbp), %rsi
    movq    -56(%rbp), %r8
    movq    -64(%rbp), %r9
    movq    -72(%rbp), %r10
    movq    -80(%rbp), %r11
    movq    -88(%rbp), %r12
    movq    -96(%rbp), %r13
    movq    -104(%rbp), %r14
    // r15 was refreshed from TLS; do not overwrite the saved return value in rax.

    movapd  -128(%rbp), %xmm0
    movapd  -144(%rbp), %xmm1
    movapd  -160(%rbp), %xmm2
    movapd  -176(%rbp), %xmm3
    movapd  -192(%rbp), %xmm4
    movapd  -208(%rbp), %xmm5
    movapd  -224(%rbp), %xmm6
    movapd  -240(%rbp), %xmm7
    movapd  -256(%rbp), %xmm8
    movapd  -272(%rbp), %xmm9
    movapd  -288(%rbp), %xmm10
    movapd  -304(%rbp), %xmm11
    movapd  -320(%rbp), %xmm12
    movapd  -336(%rbp), %xmm13
    movapd  -352(%rbp), %xmm14
    movapd  -368(%rbp), %xmm15

    addq    $ReturnSafepointStubFrameSize, %rsp
    popq    %rbp
    retq
    .seh_endproc
