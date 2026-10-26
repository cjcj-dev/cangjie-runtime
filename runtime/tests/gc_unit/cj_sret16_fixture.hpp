// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once

extern "C" void GcUnitCJSRet16Body(void* output);

#if defined(__aarch64__)
// MethodInfo::PrepareSRet does not add this buffer to the ordinary arguments
// on AArch64. ApplyCangjieMethodStub passes it in x8, including for 16B CJ
// records. Adapt only that argument, preserving SP, LR and callee-save registers.
// The body is an ordinary native function; the tail branch adds no frame.
extern "C" void GcUnitCJSRet16Entry();
#if defined(__APPLE__)
asm(".text\n.p2align 2\n.globl _GcUnitCJSRet16Entry\n"
    "_GcUnitCJSRet16Entry:\n.cfi_startproc\n"
    "mov x0, x8\n"
    "b _GcUnitCJSRet16Body\n.cfi_endproc\n");
#else
asm(".text\n.p2align 2\n.globl GcUnitCJSRet16Entry\n"
    ".type GcUnitCJSRet16Entry, %function\n"
    "GcUnitCJSRet16Entry:\n.cfi_startproc\n"
    "mov x0, x8\n"
    "b GcUnitCJSRet16Body\n.cfi_endproc\n"
    ".size GcUnitCJSRet16Entry, .-GcUnitCJSRet16Entry\n");
#endif
#else
// On x86_64, PrepareSRet adds the buffer as the first ordinary argument.
extern "C" void GcUnitCJSRet16Entry(void* output)
{
    GcUnitCJSRet16Body(output);
}
#endif
