// Win64 native caller bridge. All four C++ callees belong to the product DLL.
.text
.globl InvokeOrdinary
InvokeOrdinary:
    pushq %rbp
    movq %rsp, %rbp
    pushq %r15
    pushq %rbx
    subq $64, %rsp
    movdqu %xmm14, 32(%rsp)
    movdqu %xmm15, 48(%rsp)
    movq %rcx, %r15
    movq %r8, %rbx
    movdqu 0(%rdx), %xmm14
    movdqu 16(%rdx), %xmm15
    callq CJ_MCC_HandleSafepoint
    movdqu %xmm14, 0(%rbx)
    movdqu %xmm15, 16(%rbx)
    movdqu 32(%rsp), %xmm14
    movdqu 48(%rsp), %xmm15
    addq $64, %rsp
    popq %rbx
    popq %r15
    popq %rbp
    retq
