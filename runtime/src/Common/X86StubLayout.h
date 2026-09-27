// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#ifndef MRT_X86_STUB_LAYOUT_H
#define MRT_X86_STUB_LAYOUT_H

// Shared by the saving assembly and the stack-map location consumer.
// Like HotSpot RegisterSaver (sharedRuntime_x86_64.cpp:288), locations
// come from the same layout that emits the register saves.
#define MRT_X86_STUB_GPR_BYTES 120
#define MRT_X86_STUB_VECTOR_BYTES 264
#define MRT_X86_STUB_XMM_BASE (-(MRT_X86_STUB_GPR_BYTES + MRT_X86_STUB_VECTOR_BYTES))
#define MRT_X86_STUB_GPRS(V) \
    V(rax, RAX, -8) V(rbx, RBX, -16) V(rcx, RCX, -24) V(rdx, RDX, -32) \
    V(rdi, RDI, -40) V(rsi, RSI, -48) V(rsp, RSP, -56) V(r8, R8, -64) \
    V(r9, R9, -72) V(r10, R10, -80) V(r11, R11, -88) V(r12, R12, -96) \
    V(r13, R13, -104) V(r14, R14, -112) V(r15, R15, -120)
#define MRT_X86_STUB_XMMS(V) \
    V(xmm0, XMM0, 0) V(xmm1, XMM1, 16) V(xmm2, XMM2, 32) V(xmm3, XMM3, 48) \
    V(xmm4, XMM4, 64) V(xmm5, XMM5, 80) V(xmm6, XMM6, 96) V(xmm7, XMM7, 112) \
    V(xmm8, XMM8, 128) V(xmm9, XMM9, 144) V(xmm10, XMM10, 160) V(xmm11, XMM11, 176) \
    V(xmm12, XMM12, 192) V(xmm13, XMM13, 208) V(xmm14, XMM14, 224) V(xmm15, XMM15, 240)

#ifdef __ASSEMBLER__
// Bind each consumer offset to the actual push position at assembly time.
#define MRT_X86_STUB_PUSH(reg, id, off) \
    pushq %reg; .set mrt_stub_depth, mrt_stub_depth + 8; \
    .if -mrt_stub_depth != off; .error "stub GPR offset mismatch"; .endif; \
    .cfi_rel_offset %reg, off;
#define MRT_X86_STUB_SAVE_XMM(reg, id, off) movapd %reg, off(%rsp);
#define MRT_X86_STUB_RESTORE_XMM(reg, id, off) movapd off(%rsp), %reg;
#endif
#endif
