// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Paired with the DLL linker's -e __CJ_DllMainCRTStartupV1. The complete CRT
// entry (including the user's DllMain) must return before success is recorded.
    .text
    .p2align 4, 0x90
    .globl __CJ_DllMainCRTStartupV1
__CJ_DllMainCRTStartupV1:
    .seh_proc __CJ_DllMainCRTStartupV1
    subq $72, %rsp
    .seh_stackalloc 72
    .seh_endprologue
    movl %edx, 40(%rsp)
    callq DllMainCRTStartup
    movl %eax, 48(%rsp)
    cmpl $1, 40(%rsp)                 # DLL_PROCESS_ATTACH
    jne .Lcj_native_entry_return
    movq __CJ_NativeRegistrationV1(%rip), %rcx
    xorl %edx, %edx
    cmpl $0, 48(%rsp)
    setne %dl
    callq MRT_LibraryNativeAttachV1
.Lcj_native_entry_return:
    movl 48(%rsp), %eax
    addq $72, %rsp
    retq
    .seh_endproc
