/* Test-only adaptation of the product known-struct sret ABI to a C helper.
 * MethodInfo::ApplyCJMethodImpl -> ApplyCangjieMethodStub puts sret in x8.
 * Tail branches leave LR, SP (16-byte aligned), and all callee-saved registers
 * unchanged; the C helpers preserve the ordinary AAPCS64 callee-save set.
 */
#if defined(__aarch64__)
.text
.p2align 2
.global ReturnReflectedStruct
.type ReturnReflectedStruct, %function
ReturnReflectedStruct:
    .cfi_startproc
    mov x1, x0
    // Deliberately discard the test output into a safe writable carrier.
    // The real calloc'ed sret remains null/zero for the product boxer.
    stp x29, x30, [sp, #-32]!
    .cfi_def_cfa_offset 32
    .cfi_offset x29, -32
    .cfi_offset x30, -24
    mov x29, sp
    add x0, sp, #16
    bl ReturnReflectedStructBody
    ldp x29, x30, [sp], #32
    .cfi_def_cfa_offset 0
    .cfi_restore x29
    .cfi_restore x30
    ret
    .cfi_endproc
.size ReturnReflectedStruct, .-ReturnReflectedStruct

.p2align 2
.global ReturnKnownReflectedStruct
.type ReturnKnownReflectedStruct, %function
ReturnKnownReflectedStruct:
    .cfi_startproc
    mov x1, x0
    mov x0, x8
    b ReturnKnownReflectedStructBody
    .cfi_endproc
.size ReturnKnownReflectedStruct, .-ReturnKnownReflectedStruct
#endif
.section .note.GNU-stack,"",%progbits
