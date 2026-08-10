// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ROUTE_DOM_H
#define MRT_ROUTE_DOM_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;
class RegionInfo;

// routedom: observe whether GetRoute is asked for objects outside the mark domain.
//
// Gate (default off; product path early-return BEFORE any work):
//   MRT_GCV2_VERIFY_ROUTEDOM=1
// Positive control (invert mark predicate → name every routed object):
//   MRT_GCV2_VERIFY_ROUTEDOM_INVERT=1
// Sample cap on violation lines: MRT_GCV2_VERIFY_ROUTEDOM_MAX=<N> (default 64)
//
// Call site: RegionInfo::GetRoute after survivor gate, before RouteInfo::GetRoute(preLiveBytes).
// Predicate: RegionInfo::IsMarkedObject(obj) (unmodified). Invert only for positive control.
// On violation: log obj · region · size · TypeInfo · preLiveBytes · toAddr · nextMarkedToAddr
// Summary (atexit + optional Dump): ROUTEDOM total=N alias=M unmarked=U invert=I

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
