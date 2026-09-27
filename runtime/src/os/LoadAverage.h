// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_OS_LOAD_AVERAGE_H
#define MRT_OS_LOAD_AVERAGE_H

namespace MapleRuntime {
namespace Os {
// os::loadavg: return the number of samples, or -1 when unavailable.
int GetLoadAverage(double loadavg[], int nelem);
} // namespace Os
} // namespace MapleRuntime

#endif // MRT_OS_LOAD_AVERAGE_H
