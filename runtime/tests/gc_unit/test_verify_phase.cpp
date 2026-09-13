// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/Verify/ZVerify.h"
#include "gc_unittest.hpp"
#include <cstdlib>
#include <cstring>
using namespace MapleRuntime;
namespace {
bool StartupValue(const char* name, bool fallback)
{
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : std::strcmp(value, "1") == 0;
}
}
// z_globals.hpp:78-119 and HotSpot runtime/CommandLine flag-default tests.
GC_TEST(ZVerify, StartupFlagsMatchZgcDefaults)
{
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
    constexpr bool debug = true;
#else
    constexpr bool debug = false;
#endif
    GC_EXPECT_EQ(ZVerifyRoots, StartupValue("ZVerifyRoots", debug));
    GC_EXPECT_EQ(ZVerifyObjects, StartupValue("ZVerifyObjects", false));
    GC_EXPECT_EQ(ZVerifyMarking, StartupValue("ZVerifyMarking", debug));
    GC_EXPECT_EQ(ZVerifyRemembered, StartupValue("ZVerifyRemembered", debug));
    GC_EXPECT_EQ(ZVerifyForwarding, StartupValue("ZVerifyForwarding", false));
    GC_EXPECT_EQ(ZVerifyOops, debug && StartupValue("ZVerifyOops", false));
}
