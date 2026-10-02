#include <ptrauth.h>
#include <stdint.h>
typedef int (*code_pointer)(int);
code_pointer probe_ia(code_pointer raw, uintptr_t modifier)
{
    code_pointer signed_pointer = ptrauth_sign_unauthenticated(raw, ptrauth_key_asia, modifier);
    code_pointer stripped = ptrauth_strip(signed_pointer, ptrauth_key_asia);
    code_pointer authenticated = ptrauth_auth_function(signed_pointer, ptrauth_key_asia, modifier);
    extern void observe(code_pointer, code_pointer);
    observe(stripped, authenticated);
    return stripped;
}
code_pointer probe_ib(code_pointer raw, uintptr_t modifier)
{
    code_pointer signed_pointer = ptrauth_sign_unauthenticated(raw, ptrauth_key_asib, modifier);
    code_pointer stripped = ptrauth_strip(signed_pointer, ptrauth_key_asib);
    code_pointer authenticated = ptrauth_auth_function(signed_pointer, ptrauth_key_asib, modifier);
    extern void observe(code_pointer, code_pointer);
    observe(stripped, authenticated);
    return stripped;
}
extern int external_call(int);
int probe_nonleaf(int value) { return external_call(value) + 1; }
