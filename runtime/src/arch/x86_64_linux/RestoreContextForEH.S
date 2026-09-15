// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


// context include all callee saved register %rbx %r12 %r13 %r14 %r15 %rbp and %rsp
// stack need to be 16-byte aligned, so we should use (8 * even numbers)
#define ContextSize (8 * 8)

// This assembly function is only used in exception handling.
// It is easier to maintain the stack structure and register status by using assembly.
// Used to restore the callee saved register and frame pointer layer by layer.
// Finally, jump to the landingpad of catch pointer.
    .text
    .align 2
    .global   RestoreContextForEH
    .type     RestoreContextForEH, @function
RestoreContextForEH:
    .cfi_startproc
    // Current rsp point to the top managed method.
    pushq   %rsp
    pushq   %rbp
    .cfi_def_cfa_register %rsp
    .cfi_def_cfa_offset 16
    callq   MRT_GetTopManagedPC
    movq    0x08(%rsp), %rdi
    movq    %rax, 0x08(%rsp)

    subq    $ContextSize, %rsp
    .cfi_def_cfa_offset 80
    // Save all callee saved register to stack as the layout of context struct.
    movq    %rbx, (%rsp)
    movq    %r12, 0x08(%rsp)
    movq    %r13, 0x10(%rsp)
    movq    %r14, 0x18(%rsp)
    movq    %r15, 0x20(%rsp)
    movq    %rbp, 0x28(%rsp)
    movq    %rdi, 0x30(%rsp)

    // Current rsp point to the context struct address.
    movq    %rsp, %rdi
    callq   MRT_RestoreContext

    // rax holds the landing pad. When an uncaught exception resumes at
    // unwindPCForN2CStub, N2CStub saves rax/rdx as the C return value.
    // Leaving the landing-pad PC in rax makes C see a code address (e.g.
    // CJ_MCC_N2CStub+N) as retBuffer → SEGV_ACCERR on PROT_NONE ELF gap.
    // Zero C return regs before the jump; catch pads do not depend on them.
    movq    %rax, %r11
    xorq    %rax, %rax
    xorq    %rdx, %rdx
    xorps   %xmm0, %xmm0
    xorps   %xmm1, %xmm1

    // Restore all callee saved register.
    movq    (%rsp), %rbx
    movq    0x08(%rsp), %r12
    movq    0x10(%rsp), %r13
    movq    0x18(%rsp), %r14
    movq    0x20(%rsp), %r15
    movq    0x28(%rsp), %rbp
    movq    0x30(%rsp), %rsp

    // jump to landingpad.
    jmpq   *%r11
    .cfi_endproc
    .size RestoreContextForEH, .-RestoreContextForEH
