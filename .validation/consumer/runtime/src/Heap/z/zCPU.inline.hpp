// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zCPU.inline.hpp:32-46
#pragma once
#include "Heap/z/zCPU.hpp"

#include <cassert>
#include <thread>
#include <unistd.h>

namespace MapleRuntime {
// zCPU.inline.hpp:32-34 (os::processor_count == sysconf(_SC_NPROCESSORS_CONF), os_linux.cpp)
inline uint32_t ZCPU::count()
{
    static const uint32_t configured = [] {
#if defined(__linux__) || defined(hongmeng)
        const long value = sysconf(_SC_NPROCESSORS_CONF);
        if (value > 0) {
            return static_cast<uint32_t>(value);
        }
#endif
        const unsigned hardware = std::thread::hardware_concurrency();
        return hardware == 0 ? 1U : hardware;
    }();
    return configured;
}

// zCPU.inline.hpp:36-46
inline uint32_t ZCPU::id()
{
    assert(_affinity != nullptr && "Not initialized");

    // Fast path
    if (_affinity[_cpu]._thread == _self) {
        return _cpu;
    }

    // Slow path
    return id_slow();
}
} // namespace MapleRuntime
