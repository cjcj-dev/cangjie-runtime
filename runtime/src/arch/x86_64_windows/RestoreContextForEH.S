// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// context include all callee saved register
// stack need to be 16-byte aligned, so we should use (8 * even numbers)
#define ContextSize (8 * 30)
#define NativeHomeSize 32
#define FrameSize (ContextSize + NativeHomeSize)

// This assembly function is only used in exception handling.
// It is easier to maintain the stack structure and register status by using assembly.
// Used to restore the callee saved register and frame pointer layer by layer.
// Finally, jump to the landingpad of catch pointer.
    .def     RestoreContextForEH;
    .scl    2;
    .type   32;
    .endef
    .globl RestoreContextForEH
    .p2align    4, 0x90
RestoreContextForEH:
.seh_proc RestoreContextForEH
    // Current rsp point to the top managed method.
    movq    %rsp, %rcx
    // The helper owns [rsp, rsp + 32) as its Windows argument home space.
    // Keep the entire mutable CalleeSavedRegisterContext above that region.
    subq    $FrameSize, %rsp

    // Save all callee saved register to stack as the layout of context struct.
    movq    %rbx, NativeHomeSize(%rsp)
    movq    %rsi, NativeHomeSize+8(%rsp)
    movq    %rdi, NativeHomeSize+16(%rsp)
    movq    %r12, NativeHomeSize+24(%rsp)
    movq    %r13, NativeHomeSize+32(%rsp)
    movq    %r14, NativeHomeSize+40(%rsp)
    movq    %r15, NativeHomeSize+48(%rsp)
    movapd  %xmm6, NativeHomeSize+64(%rsp)
    movapd  %xmm7, NativeHomeSize+80(%rsp)
    movapd  %xmm8, NativeHomeSize+96(%rsp)
    movapd  %xmm9, NativeHomeSize+112(%rsp)
    movapd  %xmm10, NativeHomeSize+128(%rsp)
    movapd  %xmm11, NativeHomeSize+144(%rsp)
    movapd  %xmm12, NativeHomeSize+160(%rsp)
    movapd  %xmm13, NativeHomeSize+176(%rsp)
    movapd  %xmm14, NativeHomeSize+192(%rsp)
    movapd  %xmm15, NativeHomeSize+208(%rsp)
    movq    %rbp, NativeHomeSize+224(%rsp)
    movq    %rcx, NativeHomeSize+232(%rsp)

    leaq    NativeHomeSize(%rsp), %rcx
    callq   MRT_RestoreContext

    // See x86_64_linux/RestoreContextForEH.S: zero C return regs so N2CStub
    // does not treat the landing-pad PC as the C return value.
    // Win64: rax/rdx return; r11 is volatile scratch.
    movq    %rax, %r11
    xorq    %rax, %rax
    xorq    %rdx, %rdx
    xorps   %xmm0, %xmm0
    xorps   %xmm1, %xmm1

    // Restore all callee saved register.
    movq    NativeHomeSize(%rsp), %rbx
    movq    NativeHomeSize+8(%rsp), %rsi
    movq    NativeHomeSize+16(%rsp), %rdi
    movq    NativeHomeSize+24(%rsp), %r12
    movq    NativeHomeSize+32(%rsp), %r13
    movq    NativeHomeSize+40(%rsp), %r14
    movq    NativeHomeSize+48(%rsp), %r15
    movapd  NativeHomeSize+64(%rsp), %xmm6
    movapd  NativeHomeSize+80(%rsp), %xmm7
    movapd  NativeHomeSize+96(%rsp), %xmm8
    movapd  NativeHomeSize+112(%rsp), %xmm9
    movapd  NativeHomeSize+128(%rsp), %xmm10
    movapd  NativeHomeSize+144(%rsp), %xmm11
    movapd  NativeHomeSize+160(%rsp), %xmm12
    movapd  NativeHomeSize+176(%rsp), %xmm13
    movapd  NativeHomeSize+192(%rsp), %xmm14
    movapd  NativeHomeSize+208(%rsp), %xmm15
    movq    NativeHomeSize+224(%rsp), %rbp
    movq    NativeHomeSize+232(%rsp), %rsp

    // jump to landingpad.
    jmpq   *%r11
    .seh_endproc
