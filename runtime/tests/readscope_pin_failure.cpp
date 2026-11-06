#include <cstdio>
#include <dlfcn.h>
#include <link.h>

// Test-only native loader interposition. The real public/native producer still
// runs; only its NODELETE promotion fails, before metadata publication.
extern "C" void* dlmopen(Lmid_t ns, const char* name, int flags)
{
    if ((flags & (RTLD_NODELETE | RTLD_NOLOAD)) == (RTLD_NODELETE | RTLD_NOLOAD)) {
        std::fprintf(stderr, "TARGET native-pin-failure ns=%ld image=%s\n", static_cast<long>(ns), name ? name : "<main>");
        return nullptr;
    }
    using Open = void* (*)(Lmid_t, const char*, int);
    auto next = reinterpret_cast<Open>(dlsym(RTLD_NEXT, "dlmopen"));
    return next ? next(ns, name, flags) : nullptr;
}
