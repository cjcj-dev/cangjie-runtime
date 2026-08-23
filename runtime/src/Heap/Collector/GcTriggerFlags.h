// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_TRIGGER_FLAGS_H
#define MRT_GC_TRIGGER_FLAGS_H

#include <atomic>
#include <cstdint>

namespace MapleRuntime {

// Slim header so mark/copy TUs can read the product switches without
// pulling DecideGcTrigger (GcTrigger.h) into WCollector.cpp.
// zDirector.cpp:550-605 / z_globals.hpp:48 — ZProactive default true.
constexpr bool kGcTriggerProactiveEnabled = true;
// zDirector.cpp:470-519 + :830-833 — upgrade a would-be minor into major.
// Product off: our MAJOR is a copying full-heap GC (same trap as warmup,
// zDirector.cpp:401-424 / gctrigger4). ZGC's major is a concurrent old
// collection. Enabling this on 12-wave NW upgraded ~3000 minors and SEGV.
// Flip true only for the old-filling positive control.
constexpr bool kGcTriggerMajorAllocRateEnabled = false;
// zDirector.cpp:100-145 / :783-793 — select workers from predicted duration.
// Default off: our copying MAJOR/shared pool is not ZGC's concurrent generations.
constexpr bool kGcTriggerDynamicWorkersEnabled = false;

extern std::atomic<uint32_t> g_gcTriggerYoungWorkers;
extern std::atomic<uint32_t> g_gcTriggerOldWorkers;

} // namespace MapleRuntime
#endif // MRT_GC_TRIGGER_FLAGS_H
