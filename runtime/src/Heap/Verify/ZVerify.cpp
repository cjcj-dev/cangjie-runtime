// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/Verify/ZVerify.h"
#include <cstdlib>
#include <cstring>
namespace MapleRuntime {
namespace {
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
constexpr bool trueInDebug = true;
#else
constexpr bool trueInDebug = false;
#endif
bool Flag(const char* name, bool defaultValue)
{
    const char* value = std::getenv(name);
    return value == nullptr ? defaultValue : std::strcmp(value, "1") == 0;
}
}
const bool ZVerifyRoots = Flag("ZVerifyRoots", trueInDebug);
const bool ZVerifyObjects = Flag("ZVerifyObjects", false);
const bool ZVerifyMarking = Flag("ZVerifyMarking", trueInDebug);
const bool ZVerifyRemembered = Flag("ZVerifyRemembered", trueInDebug);
const bool ZVerifyForwarding = Flag("ZVerifyForwarding", false);
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
const bool ZVerifyOops = Flag("ZVerifyOops", false);
#else
// Upstream develop flags are unavailable in a product build.
const bool ZVerifyOops = false;
#endif

// zVerify.cpp:489-515: phase entrypoints, not a GC-wide diagnostic scene.
void ZVerify::BeforeZOperation()
{
    if (ZVerifyRoots) { RootsStrong(false); }
}
void ZVerify::AfterMark()
{
    if (ZVerifyRoots) { RootsStrong(true); }
    if (ZVerifyObjects) { Objects(false); }
}
void ZVerify::AfterWeakProcessing()
{
    if (ZVerifyRoots) {
        RootsStrong(true);
        RootsWeak();
    }
    if (ZVerifyObjects) { Objects(true); }
}
} // namespace MapleRuntime
