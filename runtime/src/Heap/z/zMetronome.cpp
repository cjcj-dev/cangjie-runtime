// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMetronome.hpp"

namespace MapleRuntime {
GcMetronome::GcMetronome(uint64_t startNs, uint64_t intervalNs )
        : startNs(startNs), intervalNs(intervalNs) {}
}
