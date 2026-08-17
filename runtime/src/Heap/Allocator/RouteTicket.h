// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ROUTE_TICKET_H
#define MRT_ROUTE_TICKET_H

#include "Base/Macros.h"

namespace MapleRuntime {
class BaseObject;
class RegionInfo;

// Domain ticket: sole mint path = RegionInfo::AdmitForRoute.
// Holding a ticket ⇔ "from object's start bit is set on this region's sealed face"
// held at mint time. See ops/design/ROUTE_DOMAIN.md §2.
//
// Criterion: code that asks for a route with an unadmitted address must not compile.
// GetRoute takes RouteTicket; BaseObject* overload is deleted in the same commit.
class RouteTicket {
public:
    BaseObject* From() const { return from_; }

private:
    friend class RegionInfo;
    friend class OptionalRouteTicket;
    explicit RouteTicket(BaseObject* f) : from_(f) {}
    BaseObject* from_;
};

// C++14 stand-in for std::optional<RouteTicket> (project builds with -std=gnu++14).
// Paired with ATTR_WARN_UNUSED on AdmitForRoute so every consumer must name the miss arm.
class OptionalRouteTicket {
public:
    OptionalRouteTicket() : hasValue_(false), from_(nullptr) {}

    explicit operator bool() const { return hasValue_; }
    bool has_value() const { return hasValue_; }

    RouteTicket value() const { return RouteTicket(from_); }

private:
    friend class RegionInfo;
    explicit OptionalRouteTicket(BaseObject* f) : hasValue_(true), from_(f) {}
    bool hasValue_;
    BaseObject* from_;
};
} // namespace MapleRuntime

#endif // MRT_ROUTE_TICKET_H
