// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "os/LoadAverage.h"

namespace MapleRuntime {
namespace Os {
// HotSpot os/windows/os_windows.cpp:5296: no load-average primitive.
int GetLoadAverage(double loadavg[], int nelem)
{
    (void)loadavg;
    (void)nelem;
    return -1;
}
} // namespace Os
} // namespace MapleRuntime
