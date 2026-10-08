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
    mov x0, x8
    b ReturnReflectedStructBody
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
