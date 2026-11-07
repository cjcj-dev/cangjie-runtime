#include <dlfcn.h>
#include <link.h>

extern "C" long readscope_null_namespace()
{
    void* handle = dlopen(nullptr, RTLD_NOW | RTLD_NOLOAD);
    Lmid_t ns = -1;
    if (!handle || dlinfo(handle, RTLD_DI_LMID, &ns) != 0) { return -1; }
    dlclose(handle);
    return ns;
}
extern "C" int readscope_global_visible()
{
    return dlsym(RTLD_DEFAULT, "readscope_namespace_value") != nullptr;
}
