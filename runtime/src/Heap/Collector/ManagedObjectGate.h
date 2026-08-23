// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MANAGED_OBJECT_GATE_H
#define MRT_MANAGED_OBJECT_GATE_H

namespace MapleRuntime {
class BaseObject;

// Leaf declaration for allocator headers which must reject stale/interior
// addresses without depending on Collector.h (and its Heap-facing graph).
// The single product implementation lives in Collector.cpp.
bool PlausibleManagedObjectGate(const char* site, BaseObject* obj);
} // namespace MapleRuntime

#endif // MRT_MANAGED_OBJECT_GATE_H
