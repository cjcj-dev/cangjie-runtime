#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <link.h>

extern "C" int dlinfo(void* handle, int request, void* result) noexcept
{
    using Function = int(*)(void*, int, void*);
    static auto next = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "dlinfo"));
    if (!next) { return -1; }
    link_map* map = nullptr;
    if (next(handle, RTLD_DI_LINKMAP, &map) == 0 && map &&
        std::strstr(map->l_name, "libcangjie-runtime.so")) {
#ifdef READSCOPE_NAMESPACE_IDENTITY_CUT
        if (request == RTLD_DI_LINKMAP) {
            std::fputs("TARGET namespace-identity-mismatch\n", stderr);
            *static_cast<link_map**>(result) = nullptr;
            return 0;
        }
#else
        if (request == RTLD_DI_LMID) {
            std::fputs("TARGET namespace-lmid-failure\n", stderr);
            return -1;
        }
#endif
    }
    return next(handle, request, result);
}
