// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_M0_EXIT_DIAGNOSTICS_H
#define MRT_M0_EXIT_DIAGNOSTICS_H

#include <cstdint>

#include "Common/TypeDef.h"
#include "StackMap/CompressedStackMap.h"

namespace MapleRuntime {
class BaseObject;

// M0 is the last non-terminating exit after relocation lookup failed. These records preserve
// enough state to distinguish no-copy (S0) from copy-published but unavailable (S1), without
// choosing a repair or changing the value returned to the mutator/retained in a root.
namespace M0ExitDiagnostics {

enum class Exit : uint8_t { RootFix = 0, ReadBarrier = 1 };

// Detailed records are intentionally bounded. Every event still contributes to the aggregate
// classifier below, and the process summary reports sampled/suppressed/total explicitly.
constexpr uint64_t kDetailedSampleLimit = 32;

#if defined(MRT_GC_UNIT_TEST_ACCESS)
struct Counts {
    uint64_t total;
    uint64_t s0;
    uint64_t s1;
    uint64_t rootFix;
    uint64_t readBarrier;
    uint64_t activeWitness;
    uint64_t retiredWitness;
    uint64_t copyPublishedWitness;
    uint64_t sampled;
    uint64_t suppressed;
};
#endif

// Installed by GCStackInfo's real frame visitors. A root-fix reached inside the scope can name
// the exact frame map that produced its RootSlot. Non-frame roots run without this scope.
class StackMapScope {
public:
    StackMapScope(bool valid, StackMapInvalidReason reason, uintptr_t startIP, uintptr_t frameIP,
                  uintptr_t frameFA);
    ~StackMapScope();

    StackMapScope(const StackMapScope&) = delete;
    StackMapScope& operator=(const StackMapScope&) = delete;

private:
    bool hadPrevious;
    bool previousValid;
    StackMapInvalidReason previousReason;
    uintptr_t previousStartIP;
    uintptr_t previousFrameIP;
    uintptr_t previousFrameFA;
};

void Note(Exit exit, BaseObject* target, const void* slot, BaseObject* holder = nullptr, uint8_t phase = 0xffu);
#if defined(MRT_GC_UNIT_TEST_ACCESS)
Counts GetCounts();
#endif

} // namespace M0ExitDiagnostics
} // namespace MapleRuntime

#endif // MRT_M0_EXIT_DIAGNOSTICS_H
