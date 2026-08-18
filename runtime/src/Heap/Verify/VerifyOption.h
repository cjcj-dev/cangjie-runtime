// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_VERIFY_OPTION_H
#define MRT_VERIFY_OPTION_H

#include <cstdint>

namespace MapleRuntime {

// Anchor: HotSpot gc/shared/verifyOption.hpp:30-39
// Purpose: make the mark-information source used by a verifier *explicit*, so the
// verifier is not tautologically true against the same data it is checking.
//
// Our mark-information inventory (see REPORT-gcvheap MARK_SOURCES):
//   MinorClosure       — TraceYoungClosure reachableObjects
//   RegionMarkBitmap   — RegionInfo::IsMarkedObject bits written during trace
//   IndependentRetrace — verifier-local recompute from roots (ValidateYoungMarking body)
//   RememberedSet      — slot set (edge bookkeeping, not object marks)
//   FullHeapEnum       — Heap::ForEachObj (enumeration, not reachability)
//
// Flip kVerifyYoungMarking (WCollector.cpp:ValidateYoungMarking) and rebuild.
// No MRT_GCV2_* env: those were cut 190 -> 3; pinned getenv is a false-negative.
constexpr bool kVerifyYoungMarking = false;

enum class VerifyMarkSource : uint8_t {
    IndependentVsBitmap = 0,
    MinorClosure,
    RegionMarkBitmap,
    IndependentRetrace,
};

// Default IndependentVsBitmap — does NOT require MinorClosure membership, so
// fullYoungScan is not tautological (gcvheap / HotSpot inventory #22).
constexpr VerifyMarkSource kVerifyMarkSource = VerifyMarkSource::IndependentVsBitmap;

const char* VerifyMarkSourceName(VerifyMarkSource source);
inline VerifyMarkSource ParseVerifyMarkSource() { return kVerifyMarkSource; }

} // namespace MapleRuntime

#endif // MRT_VERIFY_OPTION_H
