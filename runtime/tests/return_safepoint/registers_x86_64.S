// Direct bridge to the product return stub; no replacement handlers.
#ifdef __APPLE__
#define SYM(x) _##x
#else
#define SYM(x) x
#endif
.text
.globl SYM(CheckReturnStub)
SYM(CheckReturnStub):
    pushq %rbp
    movq %rsp, %rbp
    subq $512, %rsp
    movq %rbx, 32(%rsp)
    movq %rsi, 40(%rsp)
    movq %rdi, 48(%rsp)
    movq %r12, 56(%rsp)
    movq %r13, 64(%rsp)
    movq %r14, 72(%rsp)
    movq %r15, 80(%rsp)
    movdqu %xmm6, 96(%rsp)
    movdqu %xmm7, 112(%rsp)
    movdqu %xmm8, 128(%rsp)
    movdqu %xmm9, 144(%rsp)
    movdqu %xmm10, 160(%rsp)
    movdqu %xmm11, 176(%rsp)
    movdqu %xmm12, 192(%rsp)
    movdqu %xmm13, 208(%rsp)
    movdqu %xmm14, 224(%rsp)
    movdqu %xmm15, 240(%rsp)
#ifdef _WIN64
    movq %rcx, %r15
    movq %rdx, 304(%rsp)
#else
    movq %rdi, %r15
    movq %rsi, 304(%rsp)
#endif
    movq $293076993, %rax
    movq $293076994, %rdx
    movq $293076995, %rbx
    movq $293076997, %rsi
    movq $293076998, %rdi
    movq $293076999, %r12
    movq $293077000, %r13
    movq $293077001, %r14
    pcmpeqd %xmm0, %xmm0
    pcmpeqd %xmm1, %xmm1
    pcmpeqd %xmm2, %xmm2
    pcmpeqd %xmm3, %xmm3
    pcmpeqd %xmm4, %xmm4
    pcmpeqd %xmm5, %xmm5
    pcmpeqd %xmm6, %xmm6
    pcmpeqd %xmm7, %xmm7
    pcmpeqd %xmm8, %xmm8
    pcmpeqd %xmm9, %xmm9
    pcmpeqd %xmm10, %xmm10
    pcmpeqd %xmm11, %xmm11
    pcmpeqd %xmm12, %xmm12
    pcmpeqd %xmm13, %xmm13
    pcmpeqd %xmm14, %xmm14
    pcmpeqd %xmm15, %xmm15
    movq 304(%rsp), %r10
    movq %rax, 0(%r10)
    movq %rdx, 8(%r10)
    movq %rbx, 16(%r10)
    movq %rbp, 24(%r10)
    movq %rsi, 32(%r10)
    movq %rdi, 40(%r10)
    movq %r12, 48(%r10)
    movq %r13, 56(%r10)
    movq %r14, 64(%r10)
    movq %r15, 72(%r10)
    movdqu %xmm0, 320(%r10)
    movdqu %xmm1, 336(%r10)
    movdqu %xmm2, 352(%r10)
    movdqu %xmm3, 368(%r10)
    movdqu %xmm4, 384(%r10)
    movdqu %xmm5, 400(%r10)
    movdqu %xmm6, 416(%r10)
    movdqu %xmm7, 432(%r10)
    movdqu %xmm8, 448(%r10)
    movdqu %xmm9, 464(%r10)
    movdqu %xmm10, 480(%r10)
    movdqu %xmm11, 496(%r10)
    movdqu %xmm12, 512(%r10)
    movdqu %xmm13, 528(%r10)
    movdqu %xmm14, 544(%r10)
    movdqu %xmm15, 560(%r10)
    xorq %r10, %r10
    xorq %r11, %r11
    call SYM(CJ_MCC_HandleReturnSafepoint)
    movq 304(%rsp), %r10
    movq %rax, 160(%r10)
    movq %rdx, 168(%r10)
    movq %rbx, 176(%r10)
    movq %rbp, 184(%r10)
    movq %rsi, 192(%r10)
    movq %rdi, 200(%r10)
    movq %r12, 208(%r10)
    movq %r13, 216(%r10)
    movq %r14, 224(%r10)
    movq %r15, 232(%r10)
    movdqu %xmm0, 832(%r10)
    movdqu %xmm1, 848(%r10)
    movdqu %xmm2, 864(%r10)
    movdqu %xmm3, 880(%r10)
    movdqu %xmm4, 896(%r10)
    movdqu %xmm5, 912(%r10)
    movdqu %xmm6, 928(%r10)
    movdqu %xmm7, 944(%r10)
    movdqu %xmm8, 960(%r10)
    movdqu %xmm9, 976(%r10)
    movdqu %xmm10, 992(%r10)
    movdqu %xmm11, 1008(%r10)
    movdqu %xmm12, 1024(%r10)
    movdqu %xmm13, 1040(%r10)
    movdqu %xmm14, 1056(%r10)
    movdqu %xmm15, 1072(%r10)
    movdqu 96(%rsp), %xmm6
    movdqu 112(%rsp), %xmm7
    movdqu 128(%rsp), %xmm8
    movdqu 144(%rsp), %xmm9
    movdqu 160(%rsp), %xmm10
    movdqu 176(%rsp), %xmm11
    movdqu 192(%rsp), %xmm12
    movdqu 208(%rsp), %xmm13
    movdqu 224(%rsp), %xmm14
    movdqu 240(%rsp), %xmm15
    movq 32(%rsp), %rbx
    movq 40(%rsp), %rsi
    movq 48(%rsp), %rdi
    movq 56(%rsp), %r12
    movq 64(%rsp), %r13
    movq 72(%rsp), %r14
    movq 80(%rsp), %r15
    addq $512, %rsp
    popq %rbp
    ret
