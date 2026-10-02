// Ordinary Apple arm64; no arm64e ABI or ptrauth API.
.text
.p2align 2
.globl _pac1481_produce
// x0 = independently relocated raw instruction PC, x1 = output (3 uint64_t).
// Leaf: x30 and SP are untouched. IA, modifier zero, via PACIA1716 hint.
_pac1481_produce:
    str x0, [x1]
    mov x16, #0
    mov x17, x0
    str x16, [x1, #16]
    hint #0x8
    str x17, [x1, #8]
    ret

.p2align 2
.globl _pac1481_outside
_pac1481_outside:
    ret
