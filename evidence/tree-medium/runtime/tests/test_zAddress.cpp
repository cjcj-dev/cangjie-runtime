#include "Heap/z/zAddress.hpp"
#include <cstdio>
using namespace MapleRuntime;
static_assert(ZPointerReservedShift == 0, "reserved low");
static_assert(ZPointerRememberedShift == 4, "rr");
static_assert(ZPointerMarkedShift == 6, "FFmmMM");
static_assert(ZPointerRemappedShift == 12, "RRRR");
static_assert(ZPointerLoadMetadataMask == ZPointerRemappedMask, "load meta");
static_assert(ZPointerAllMetadataMask == ZPointerStoreMetadataMask, "all meta");
static_assert(ZPointerLoadShiftTable[1] == static_cast<int>(ZPointerRemappedShift + 1), "remap00 shift");
static_assert(ZPointerLoadShiftTable[8] == static_cast<int>(ZPointerRemappedShift + 4), "remap11 shift");
int main()
{
    std::puts("layout_ok");
    return 0;
}
