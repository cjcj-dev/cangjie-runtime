#include "Cangjie.h"
#include <cstdio>

static bool Check(bool ok, const char* name)
{
    std::printf("ASSERT %s %s\n", name, ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    return ok;
}

int main(int argc, char** argv)
{
    if (argc != 3) { return 2; }
    RuntimeParam param {};
    param.coParam.processorNum = 2;
    if (!Check(InitCJRuntime(&param) == E_OK, "runtime-init")) { return 2; }
    if (!Check(LoadCJLibraryWithInit(argv[1]) == E_OK, "first-load")) { return 2; }
    if (!Check(FindCJSymbol(argv[1], argv[2]) != nullptr, "first-symbol")) { return 2; }
    if (!Check(UnloadCJLibrary(argv[1]) == E_OK, "first-unload")) { return 1; }
    if (!Check(FindCJSymbol(argv[1], argv[2]) == nullptr, "symbol-removed")) { return 1; }
    std::puts("TARGET public-reopen"); std::fflush(stdout);
    if (!Check(LoadCJLibraryWithInit(argv[1]) == E_OK, "reopen")) { return 1; }
    if (!Check(FindCJSymbol(argv[1], argv[2]) != nullptr, "reopened-symbol")) { return 1; }
    if (!Check(UnloadCJLibrary(argv[1]) == E_OK, "second-unload")) { return 1; }
    return Check(FiniCJRuntime() == E_OK, "runtime-finalize") ? 0 : 1;
}
