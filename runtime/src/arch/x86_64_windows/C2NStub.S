// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#define C2NStubFrameSize           (8 * 46)

// On execution of "bl MCC_C2NStub", the frame layout of stack(growing downwards) looks like:
// Nativefunc address will be passed to %rax after MCC_C2NStub is built.
//                 |  ...         |
//                 |  ra          | return address for the caller of MCC_C2NStub
// caller rbp  --> |  rbp         |
//                 |  ...         |
//                 | arg7         |
//                 | arg6         |
//                 | arg5         |
//                 | arg4         |
//                 | shadow space |
//                 | shadow space |
//                 | shadow space |
//                 | shadow space |
//                 | callee addr  |
// caller rsp  --> | cpStackSize  |

// the frame layout of stack(growing downwards) after MCC_C2NStub frame is built looks like:
//                 |  ...         |
//                 |  ra          | return address for the caller of MCC_C2NStub
// caller rbp  --> |  rbp         |
//                 |  ...         |
//                 | arg7         |
//                 | arg6         |
//                 | arg5         |
//                 | arg4         |
//                 | shadow space |
//                 | shadow space |
//                 | shadow space |
//                 | shadow space |
//                 | callee addr  |
// caller rsp  --> | cpStackSize  |
//                 | ra           |
//   stub rbp  --> | caller rbp   | <== C2NStubStart
// callee register | %r15         |
//                 | %r14         |
//                 | %r13         |
//                 | %r12         |
//                 | %rdi         |
//                 | %rsi         |
//                 | %rbx         |
// arg0            | %rcx         |
// arg1            | %rdx         |
// arg2            | %r8          |
// arg3            | %r9          |
//                 | %rax         |
//                 | %xmm0(high)  |
//                 | %xmm0(low)   |
//                 | %xmm1(high)  |
//                 | %xmm1(low)   |
//                 | %xmm2(high)  |
//                 | %xmm2(low)   |
//                 | %xmm3(high)  |
//                 | %xmm3(low)   |
//                 | %xmm6(high)  |
//                 | %xmm6(low)   |
//                 | %xmm7(high)  |
//                 | %xmm7(low)   |
//                 | %xmm8(high)  |
//                 | %xmm8(low)   |
//                 | %xmm9(high)  |
//                 | %xmm9(low)   |
//                 | %xmm10(high) |
//                 | %xmm10(low)  |
//                 | %xmm11(high) |
//                 | %xmm11(low)  |
//                 | %xmm12(high) |
//                 | %xmm12(low)  |
//                 | %xmm13(high) |
//                 | %xmm13(low)  |
//                 | %xmm14(high) |
//                 | %xmm14(low)  |
//                 | %xmm15(high) |
//                 | %xmm15(low)  |
//                 | entersafe    | <== C2NStubFrameSize
//                 | ....         | <== copy caller Stack start here
//                 | arg7         |
//                 | arg6         |
//                 | arg5         |
//                 | arg4         |
//                 | shadow space |
//                 | shadow space |
//                 | shadow space |
//   stub rsp  --> | shadow space | <== MCC_C2NStub frame ends at here
    .def CJ_MCC_C2NStub
    .scl 2
    .type 32
    .endef
    .global CJ_MCC_C2NStub
    .p2align    4, 0x90
CJ_MCC_C2NStub:
    .seh_proc CJ_MCC_C2NStub
    pushq	%rbp
    .seh_pushreg %rbp
    movq    %rsp, %rbp
    // Preserve the managed RBP-relative register slots with real pushes.
    // All PUSH_NONVOL operations precede allocation and frame establishment.
    pushq   %r15
    .seh_pushreg %r15
    pushq   %r14
    .seh_pushreg %r14
    pushq   %r13
    .seh_pushreg %r13
    pushq   %r12
    .seh_pushreg %r12
    pushq   %rdi
    .seh_pushreg %rdi
    pushq   %rsi
    .seh_pushreg %rsi
    pushq   %rbx
    .seh_pushreg %rbx
    subq    $(C2NStubFrameSize - 56), %rsp
    .seh_stackalloc C2NStubFrameSize - 56
    movq    %rsp, %rsi
    .seh_setframe %rsi, 0

    // save args to stack
    movq    %rcx,  -64(%rbp)
    movq    %rdx,  -72(%rbp)
    movq    %r8,   -80(%rbp)
    movq    %r9,   -88(%rbp)
    movq    %rax,  -96(%rbp)

    movapd  %xmm0, -112(%rbp)
    movapd  %xmm1, -128(%rbp)
    movapd  %xmm2, -144(%rbp)
    movapd  %xmm3, -160(%rbp)

    // save callee-saved to stack
    movapd  %xmm6, -176(%rbp)
    .seh_savexmm %xmm6, C2NStubFrameSize - 176
    movapd  %xmm7, -192(%rbp)
    .seh_savexmm %xmm7, C2NStubFrameSize - 192
    movapd  %xmm8, -208(%rbp)
    .seh_savexmm %xmm8, C2NStubFrameSize - 208
    movapd  %xmm9, -224(%rbp)
    .seh_savexmm %xmm9, C2NStubFrameSize - 224
    movapd  %xmm10, -240(%rbp)
    .seh_savexmm %xmm10, C2NStubFrameSize - 240
    movapd  %xmm11, -256(%rbp)
    .seh_savexmm %xmm11, C2NStubFrameSize - 256
    movapd  %xmm12, -272(%rbp)
    .seh_savexmm %xmm12, C2NStubFrameSize - 272
    movapd  %xmm13, -288(%rbp)
    .seh_savexmm %xmm13, C2NStubFrameSize - 288
    movapd  %xmm14, -304(%rbp)
    .seh_savexmm %xmm14, C2NStubFrameSize - 304
    movapd  %xmm15, -320(%rbp)
    .seh_savexmm %xmm15, C2NStubFrameSize - 320
    .seh_endprologue

    // The lowest 32 bytes of the fixed frame are native helper home space.

    callq   CJ_CJThreadStackGuardGet
    // rbx = cpStackSize
    movq    16(%rbp), %rbx
    addq    %rbx, %rax
    cmpq    %rax, %rsp
    jbe     .L_stack_check_fail

    movq    %rbp, %rdx
    // Get the current pc address through the following two assembly instructions.
    call    .L_current_pc
.L_current_pc:
     .global unwindPCForC2NStub
unwindPCForC2NStub:
    pop     %rcx
    movq    %r15, %r8
    callq   MRT_C2N_Enter      // merged: SaveC2NContext + EnterSaferegion
    movq    %rax, -328(%rbp)

    // rbx = cpStackSize, r12 = calleeAddr
    movq    16(%rbp), %rbx
    movq    24(%rbp), %r12

    // Let the r14 register(current rbp) point to the starting point of the stack to be copied
    movq    %rbp, %r14
    addq    $32,  %r14
    addq    %rbx, %r14
    // Let the r13 register(current rbp) point to the ending point of the stack to be copied
    movq    %rbp, %r13
    addq    $64,  %r13

    // copy arg4, arg5, ...(if existed)
.L_copy:
    cmpq    %r13,    %r14
    jle     .L_copy_end
    subq    $8,      %r14
    movq    0(%r14), %r15
    pushq   %r15
    jmp     .L_copy
.L_copy_end:

    // Outgoing argument/home allocation is dynamic body state. RSI anchors
    // unwind to the fixed frame; loop iterations are not prologue saves.
    pushq   %r15
    pushq   %r15
    pushq   %r15
    pushq   %r15

    // Prepare arg0 to arg3
    movq    -64(%rbp),  %rcx
    movq    -72(%rbp),  %rdx
    movq    -80(%rbp),  %r8
    movq    -88(%rbp),  %r9
    movq    -96(%rbp),  %rax
    movapd  -112(%rbp), %xmm0
    movapd  -128(%rbp), %xmm1
    movapd  -144(%rbp), %xmm2
    movapd  -160(%rbp), %xmm3

    callq   *%r12

    // keep potential return value
    // rax is 1st return register, no 2nd return register in Windows
    // xmm0 are used to return floating point arguments
    movq    %rax,  -64(%rbp)
    movapd  %xmm0, -80(%rbp)

    // Call merged leave function: LeaveSaferegion + DeleteC2NContext + ThrowPendingException
    movq    -328(%rbp), %rcx
    callq   MRT_C2N_Leave      // return: thread local data
    movq    %rax, -8(%rbp)
    .global unwindPCForTPE
unwindPCForTPE:
    movq    %rbp, %rcx
    movl    $16, %edx
    callq   MRT_CompleteWindowsExceptionReturn

    // set potential return value
    movq    -64(%rbp), %rax
    movapd  -80(%rbp), %xmm0

    // Both normal and stack-check exits restore from the fixed frame.
.L_stack_check:
    movapd  -176(%rbp), %xmm6
    movapd  -192(%rbp), %xmm7
    movapd  -208(%rbp), %xmm8
    movapd  -224(%rbp), %xmm9
    movapd  -240(%rbp), %xmm10
    movapd  -256(%rbp), %xmm11
    movapd  -272(%rbp), %xmm12
    movapd  -288(%rbp), %xmm13
    movapd  -304(%rbp), %xmm14
    movapd  -320(%rbp), %xmm15

    // A legal Win64 epilogue: frame-relative LEA, POPs, private RET cleanup.
    // Do not relocate the caller's frame/return address before unwinding it.
    leaq    (C2NStubFrameSize - 56)(%rsi), %rsp
    popq    %rbx
    popq    %rsi
    popq    %rdi
    popq    %r12
    popq    %r13
    popq    %r14
    popq    %r15
    popq    %rbp
    retq    $16
.L_stack_check_fail:
    call    MCC_ThrowStackOverflowError
    callq   MRT_GetThreadLocalData
    movq    %rax, -8(%rbp)
    movq    %rax, -64(%rbp)
    movapd  %xmm0, -80(%rbp)
    movq    %rbp, %rcx
    movl    $16, %edx
    callq   MRT_CompleteWindowsExceptionReturn
    movq    -64(%rbp), %rax
    movapd  -80(%rbp), %xmm0
    jmp     .L_stack_check
    .seh_endproc
