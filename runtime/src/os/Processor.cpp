// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "os/Processor.h"
#include "Base/Log.h"
#include <algorithm>
#include <mutex>
#include <thread>
#if defined(__linux__) || defined(hongmeng)
#include <sched.h>
#endif
namespace MapleRuntime {
namespace OS {
namespace {
std::once_flag initialized;
uint32_t initialActiveProcessors = 0;
}
void InitializeProcessorCount()
{
    std::call_once(initialized, [] {
        uint32_t count = std::max(1u, std::thread::hardware_concurrency());
#if defined(__linux__) || defined(hongmeng)
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        if (sched_getaffinity(0, sizeof(cpus), &cpus) == 0 && CPU_COUNT(&cpus) > 0) {
            count = static_cast<uint32_t>(CPU_COUNT(&cpus));
        }
#endif
        initialActiveProcessors = count;
    });
}
uint32_t InitialActiveProcessorCount()
{
    CHECK(initialActiveProcessors > 0);
    return initialActiveProcessors;
}
}
}
