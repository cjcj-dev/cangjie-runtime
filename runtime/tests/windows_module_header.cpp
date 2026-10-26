// Compile the product header before any C++ library includes from a consumer.
// In particular, no test framework may provide <string> before this header.
#include "os/Windows/WinModuleManager.h"

void WindowsModuleHeaderConsumer(MapleRuntime::WinModule* module)
{
    MapleRuntime::WinModuleManager manager;
    (void)manager;
    (void)MapleRuntime::WinModuleHash{}(module);
}
