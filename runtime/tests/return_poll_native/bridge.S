// Native-to-managed ABI bridge only. llc owns poll and return metadata.
#ifdef __APPLE__
#define SYMBOL(x) _##x
#else
#define SYMBOL(x) x
#endif
.text
.globl SYMBOL(Invoke)
#ifdef __aarch64__
.p2align 2
SYMBOL(Invoke):
 stp x29, x30, [sp, #-32]!
 mov x29, sp
 str x28, [sp, #16]
 mov x28, x0
 mov x0, x1
 bl SYMBOL(ref_ret)
 ldr x28, [sp, #16]
 ldp x29, x30, [sp], #32
 ret
#else
SYMBOL(Invoke):
 pushq %rbp
 movq %rsp, %rbp
 pushq %r15
#ifdef _WIN32
 subq $40, %rsp
 movq %rcx, %r15
 movq %rdx, %rcx
#else
 subq $8, %rsp
 movq %rdi, %r15
 movq %rsi, %rdi
#endif
 callq SYMBOL(ref_ret)
#ifdef _WIN32
 addq $40, %rsp
#else
 addq $8, %rsp
#endif
 popq %r15
 popq %rbp
 retq
#endif
