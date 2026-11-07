#include "Cangjie.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

int main(int argc, char** argv)
{
    if (argc != 3) { return 2; }
    const bool reject = std::strcmp(argv[2], "reject") == 0;
    const int loaded = LoadCJLibrary(argv[1]);
    void* symbol = FindCJSymbol(argv[1], "readscope_namespace_value");
    const bool expected = reject ? loaded != E_OK && symbol == nullptr : loaded == E_OK && symbol != nullptr;
    std::printf("ASSERT namespace-detection-%s %s\n", reject ? "fail-closed" : "restored", expected ? "PASS" : "FAIL");
    void* control = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
    const bool independent = control && dlclose(control) == 0;
    std::printf("ASSERT independent-native-load %s\n", independent ? "PASS" : "FAIL");
    return expected && independent ? 0 : 1;
}
