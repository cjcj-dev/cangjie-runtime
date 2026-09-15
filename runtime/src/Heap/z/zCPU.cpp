// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zCPU.cpp:24-66
#include "Heap/z/zCPU.inline.hpp"

#include <sched.h>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zUtils.inline.hpp"

namespace MapleRuntime {
#define ZCPU_UNKNOWN_AFFINITY (reinterpret_cast<const void*>(-1))
#define ZCPU_UNKNOWN_SELF     (reinterpret_cast<const void*>(-2))

ZCPU::PaddedZCPUAffinity*   ZCPU::_affinity = nullptr;
thread_local const void*    ZCPU::_self     = ZCPU_UNKNOWN_SELF;
thread_local uint32_t       ZCPU::_cpu      = 0;

// zCPU.cpp:38-52 (PaddedArray::create_unfreeable == cache-line padded unfreeable block)
void ZCPU::initialize()
{
    assert(_affinity == nullptr && "Already initialized");
    const uint32_t ncpus = count();

    _affinity = reinterpret_cast<PaddedZCPUAffinity*>(
        ZUtils::alloc_aligned_unfreeable(ZCacheLineSize, sizeof(PaddedZCPUAffinity) * ncpus));

    for (uint32_t i = 0; i < ncpus; i++) {
        _affinity[i]._thread = ZCPU_UNKNOWN_AFFINITY;
    }

    VLOG(REPORT, "CPUs: %u total, %u available", count(),
         static_cast<unsigned>(sysconf(_SC_NPROCESSORS_ONLN) > 0 ? sysconf(_SC_NPROCESSORS_ONLN) : 1));
}

// zCPU.cpp:54-66
uint32_t ZCPU::id_slow()
{
    // Set current thread
    if (_self == ZCPU_UNKNOWN_SELF) {
        _self = &_cpu;
    }

    // Set current CPU (os::processor_id: os_linux.cpp sched_getcpu; invalid ids fold to 0)
#if defined(__linux__) || defined(hongmeng)
    const int cpu = sched_getcpu();
    _cpu = (cpu >= 0 && static_cast<uint32_t>(cpu) < count()) ? static_cast<uint32_t>(cpu) : 0;
#else
    _cpu = 0;
#endif

    // Update affinity table
    _affinity[_cpu]._thread = _self;

    return _cpu;
}
} // namespace MapleRuntime
