// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "os/LoadAverage.h"

#include <cstdlib>

namespace MapleRuntime {
namespace Os {
// HotSpot os/linux/os_linux.cpp:5229; Bionic exposes getloadavg at API 29.
int GetLoadAverage(double loadavg[], int nelem)
{
#if defined(__ANDROID__) && __ANDROID_API__ < 29
    // Like HotSpot's unsupported OS implementation, report unavailable.
    (void)loadavg;
    (void)nelem;
    return -1;
#else
    return ::getloadavg(loadavg, nelem);
#endif
}
} // namespace Os
} // namespace MapleRuntime
