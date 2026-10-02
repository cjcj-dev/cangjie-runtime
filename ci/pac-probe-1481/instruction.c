#include <stdint.h>
/* The fixed-register read/write operand tells clang that LR is overwritten.
 * Clang must preserve the caller LR in the function prologue/epilogue.
 * No SP write, call, or execution is permitted in this compile-only batch. */
uintptr_t probe_xpaclri(uintptr_t input)
{
    register uintptr_t lr __asm__("x30") = input;
    __asm__ volatile("hint #0x7" : "+r"(lr));
    return lr;
}
uintptr_t probe_ia_sp_roundtrip(uintptr_t input)
{
    register uintptr_t lr __asm__("x30") = input;
    __asm__ volatile("paciasp\n\tautiasp" : "+r"(lr) : : "x16", "x17");
    return lr;
}
