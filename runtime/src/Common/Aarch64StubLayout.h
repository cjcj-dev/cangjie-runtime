// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#ifndef MRT_AARCH64_STUB_LAYOUT_H
#define MRT_AARCH64_STUB_LAYOUT_H

// The assembler and RegisterMap consume one save-area layout, as in
// HotSpot RegisterSaver (sharedRuntime_x86_64.cpp:288-303).
#define MRT_AARCH64_STUB_FRAME_BYTES 0x310
#define MRT_AARCH64_STUB_CALLEE_BASE 0x20
#define MRT_AARCH64_STUB_GPRS(V) \
    V(x0, X0, 0, 0x10) \
    V(x1, X1, 1, 0x18) \
    V(x2, X2, 2, 0x20) \
    V(x3, X3, 3, 0x28) \
    V(x4, X4, 4, 0x30) \
    V(x5, X5, 5, 0x38) \
    V(x6, X6, 6, 0x40) \
    V(x7, X7, 7, 0x48) \
    V(x8, X8, 8, 0x50) \
    V(x9, X9, 9, 0x58) \
    V(x10, X10, 10, 0x60) \
    V(x11, X11, 11, 0x68) \
    V(x12, X12, 12, 0x70) \
    V(x13, X13, 13, 0x78) \
    V(x14, X14, 14, 0x80) \
    V(x15, X15, 15, 0x88) \
    V(x16, X16, 16, 0x90) \
    V(x17, X17, 17, 0x98) \
    V(x18, X18, 18, 0xa0) \
    V(x19, X19, 19, 0xa8) \
    V(x20, X20, 20, 0xb0) \
    V(x21, X21, 21, 0xb8) \
    V(x22, X22, 22, 0xc0) \
    V(x23, X23, 23, 0xc8) \
    V(x24, X24, 24, 0xd0) \
    V(x25, X25, 25, 0xd8) \
    V(x26, X26, 26, 0xe0) \
    V(x27, X27, 27, 0xe8) \
    V(x28, X28, 28, 0xf0) \
    V(x29, X29, 29, 0xf8)
#define MRT_AARCH64_STUB_CALLEE_GPRS(V) \
    V(x19, X19, 19, 0x20) \
    V(x20, X20, 20, 0x28) \
    V(x21, X21, 21, 0x30) \
    V(x22, X22, 22, 0x38) \
    V(x23, X23, 23, 0x40) \
    V(x24, X24, 24, 0x48) \
    V(x25, X25, 25, 0x50) \
    V(x26, X26, 26, 0x58) \
    V(x27, X27, 27, 0x60) \
    V(x28, X28, 28, 0x68)

#ifdef __ASSEMBLER__
#ifdef __APPLE__
#define MRT_AARCH64_ASM_SEPARATOR %%
#else
#define MRT_AARCH64_ASM_SEPARATOR ;
#endif
#define MRT_AARCH64_ALL_SLOT(reg, id, index, off) \
    .equ .Lmrt_a64_all_ ## reg, off MRT_AARCH64_ASM_SEPARATOR
#define MRT_AARCH64_CALLEE_SLOT(reg, id, index, off) \
    .equ .Lmrt_a64_callee_ ## reg, off MRT_AARCH64_ASM_SEPARATOR
MRT_AARCH64_STUB_GPRS(MRT_AARCH64_ALL_SLOT)
MRT_AARCH64_STUB_CALLEE_GPRS(MRT_AARCH64_CALLEE_SLOT)
#undef MRT_AARCH64_ALL_SLOT
#undef MRT_AARCH64_CALLEE_SLOT
// A paired store's second slot is implicit in the instruction. Validate the
// table used by this saving stub; unrelated save areas remain independent.
#define MRT_AARCH64_CHECK_ALL_SLOT(reg, id, index, off) \
    .if off != 16 + 8 * index MRT_AARCH64_ASM_SEPARATOR \
    .error "aarch64 all-register slot mismatch" MRT_AARCH64_ASM_SEPARATOR \
    .endif MRT_AARCH64_ASM_SEPARATOR
#define MRT_AARCH64_CHECK_CALLEE_SLOT(reg, id, index, off) \
    .if off != MRT_AARCH64_STUB_CALLEE_BASE + 8 * (index - 19) MRT_AARCH64_ASM_SEPARATOR \
    .error "aarch64 callee-register slot mismatch" MRT_AARCH64_ASM_SEPARATOR \
    .endif MRT_AARCH64_ASM_SEPARATOR
#endif
#endif
