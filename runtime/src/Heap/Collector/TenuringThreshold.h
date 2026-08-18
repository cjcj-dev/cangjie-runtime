// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_TENURING_THRESHOLD_H
#define MRT_TENURING_THRESHOLD_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "Heap/Allocator/PageAge.h"

namespace MapleRuntime {

// Whether the product *acts* on the computed threshold. Off for now: with it on,
// natural_wave_notime failed 3 of 3 (one 300s timeout, one SIGSEGV, one SIGABRT) where the same
// tree with it off runs clean, so keeping young survivors young is exposing something the rest of
// the collector is not ready for. The threshold itself is still computed and still tested -- see
// ComputeTenuringThreshold below, which no longer consults this flag.
//
// That separation is the point. The first version short-circuited the computation on this flag, so
// turning the policy off also turned off the three tests that check the arithmetic, and the switch
// could not be used to isolate the failure it exists for. A policy switch belongs at the call site,
// not inside the function whose result the policy uses.
constexpr bool kPageAgeAdaptiveTenuring = false;
constexpr uint32_t kMaxTenuringThreshold = untype(PageAge::survivor14);

struct TenuringInputs {
    size_t liveByAge[kPageAgeCount]{};
    size_t youngGarbage = 0;
    size_t youngAllocated = 0;
    size_t softMaxCapacity = 0;
    bool promoteAll = false;
};

inline uint32_t ComputeTenuringThreshold(const TenuringInputs& in)
{
    if (in.promoteAll) {
        return 0;
    }

    size_t youngLiveTotal = 0;
    size_t youngLiveLast = 0;
    double lifeExpectancySum = 0.0;
    uint32_t lifeExpectancySamples = 0;
    uint32_t lastPopulatedAge = 0;

    for (PageAge age : kPageAgeRangeAll) {
        const size_t youngLive = in.liveByAge[untype(age)];
        if (youngLive > 0) {
            lastPopulatedAge = untype(age);
            if (youngLiveLast > 0) {
                lifeExpectancySum += static_cast<double>(youngLive) / static_cast<double>(youngLiveLast);
                ++lifeExpectancySamples;
            }
        }
        youngLiveTotal += youngLive;
        youngLiveLast = youngLive;
    }

    if (youngLiveTotal == 0) {
        return 0;
    }

    const double lifeExpectancy =
        lifeExpectancySamples == 0 ? 1.0 : lifeExpectancySum / static_cast<double>(lifeExpectancySamples);
    const double lifeDecayFactor = 1.0 / lifeExpectancy;
    const double residencyReciprocal =
        in.softMaxCapacity == 0 ? 1.0 :
        static_cast<double>(in.softMaxCapacity) / static_cast<double>(youngLiveTotal);
    const double residencyFactor = std::max(residencyReciprocal, 1.0);
    const double allocatedGarbageRatio =
        static_cast<double>(in.youngAllocated) / static_cast<double>(in.youngGarbage + 1);
    const double youngLog = std::max(std::min(allocatedGarbageRatio, 1.0) * 16.0, 2.0);
    const double logResidency = std::log(residencyFactor) / std::log(youngLog);
    const double thresholdRaw = lifeDecayFactor * logResidency;

    const uint32_t upperBound = std::min(lastPopulatedAge + 1u, kMaxTenuringThreshold);
    const uint32_t lowerBound = std::min(1u, upperBound);
    const uint32_t rounded = static_cast<uint32_t>(std::llround(thresholdRaw));
    return std::min(std::max(rounded, lowerBound), upperBound);
}

inline PageAge ComputeToAge(PageAge fromAge, uint32_t tenuringThreshold)
{
    if (fromAge == PageAge::old) {
        return PageAge::old;
    }
    if (untype(fromAge) >= tenuringThreshold) {
        return PageAge::old;
    }
    return to_pageage(untype(fromAge) + 1);
}

inline bool ShouldPromoteAge(uint8_t youngAge, uint32_t tenuringThreshold)
{
    return ComputeToAge(to_pageage(youngAge), tenuringThreshold) == PageAge::old;
}

} // namespace MapleRuntime

#endif
