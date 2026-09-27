// Native caller preserves the C ABI's register used for managed thread data.
// Each call resolves the actual CJ_MCC symbol in the product dylib.
.text
.macro Invoke name
.globl _Invoke\name
.p2align 2
_Invoke\name:
#ifdef __aarch64__
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x28, [sp, #16]
    bl _CJ_MCC_PackageInit\name
    ldr x28, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
#else
    pushq %rbp
    movq %rsp, %rbp
    pushq %r15
    subq $8, %rsp
    callq _CJ_MCC_PackageInit\name
    addq $8, %rsp
    popq %r15
    popq %rbp
    retq
#endif
.endm
Invoke Begin
Invoke Complete
Invoke Fail
Invoke Abort
