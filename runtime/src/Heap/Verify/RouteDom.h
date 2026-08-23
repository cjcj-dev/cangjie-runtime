// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ⛔ HOLLOWED — the implementation in the matching .cpp is all no-ops: Enabled() returns false and
// every sink body is empty.  The gate documented below therefore emits nothing, so a zero taken from
// it is a false negative, not evidence that the arm never fires.  The contract, the gate name and the
// product call sites were all left intact when the bodies were removed, which is precisely what makes
// this readable as a live instrument.  Restore the sink you need first -- PermWhoAdmit.cpp shows the
// shape: a compile-time constant gate (the campaign cut MRT_GCV2_* from 190 to 3) plus a line on the
// zero case so a zero cannot be read as a dead probe.  Guard: runtime/tests/check_diag_not_hollow.py
#ifndef MRT_ROUTE_DOM_H
#define MRT_ROUTE_DOM_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;
class RegionInfo;

// routedom: observe geometric GetRoute domain membership at object-start granularity.
//
// Gate (default off; product path early-return BEFORE any work):
//   MRT_GCV2_VERIFY_ROUTEDOM=1
// Positive control (invert mark predicate → name every routed object as sample):
//   MRT_GCV2_VERIFY_ROUTEDOM_INVERT=1
// Sample cap on detail lines: MRT_GCV2_VERIFY_ROUTEDOM_MAX=<N> (default 64)
//
// Call site: RegionInfo::GetRoute AFTER survivor gate, after RouteInfo::GetRoute(preLiveBytes).
// Classification of every geometric route:
//   START     — fromObj is the start of a marked object
//   INTERIOR  — fromObj falls inside a marked object's [start, start+size) but is not start
// Summary: ROUTEDOM total=N start=S interior=I
// INTERIOR samples also log hostStart · hostDelta · hostTip · hostToAddr

namespace RouteDom {

bool Enabled();

// Invoked only at the geometric GetRoute call site when gate is on.
// region is the ghost-from region owning fromObj; preLiveBytes/toAddr already computed.
void NoteRoute(RegionInfo* region, BaseObject* fromObj, uint64_t preLiveBytes, uintptr_t toAddr);

// Process-exit / end-of-forward summary line.
void DumpSummary();

} // namespace RouteDom
} // namespace MapleRuntime

#endif // MRT_ROUTE_DOM_H
