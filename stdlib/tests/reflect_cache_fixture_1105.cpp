// Test-only serial metadata identity fixture. No runtime entry or layout change.
#include "Interpreter/RuntimeTypes.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
extern "C" DYN_TypeInfo* CJ_MCC_getTypeByQualifiedName(char*);
static DYN_TypeInfo* types[2];
static uint32_t saved[2];
static_assert(offsetof(DYN_TypeInfo, uuid) == 24);
extern "C" int32_t ReflectFixtureInit()
{
    char first[] = "reflectcache.First", second[] = "reflectcache.Second";
    types[0] = CJ_MCC_getTypeByQualifiedName(first);
    types[1] = CJ_MCC_getTypeByQualifiedName(second);
    if (!types[0] || !types[1] || types[0] == types[1]) return 1;
    for (int i = 0; i < 2; ++i) saved[i] = types[i]->uuid;
    if (!saved[0] || !saved[1] || saved[0] == saved[1]) return 2;
    std::printf("SERIAL_FIXTURE pointers=%p,%p uuid=%u,%u\n", types[0], types[1], saved[0], saved[1]);
    return 0;
}
extern "C" void ReflectFixtureSet(int32_t index, uint32_t value)
{
    if (index < 0 || index > 1 || !types[index]) std::abort();
    types[index]->uuid = value;
}
extern "C" void ReflectFixtureRestore()
{
    for (int i = 0; i < 2; ++i) if (types[i]) types[i]->uuid = saved[i];
}
