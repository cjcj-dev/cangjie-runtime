// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_VERIFY_HEAP_H
#define MRT_VERIFY_HEAP_H

#include <cstddef>
#include <unordered_set>

namespace MapleRuntime {

class BaseObject;
//
// Invariant H (per live object o walked via Heap::ForEachObj):
//   H1  o->IsValidObject()
//   H2  tip = o->GetTypeInfo() is not a heap address; preferably in TypeInfoManager
//       mmap OR other non-heap; and tip->IsVaildType()
//   H3  each ref field is null or points at an object that itself satisfies H1+H2+H4
//   H4  holder (and H3 target) region is not free/garbage
//
// Gate (default off): unified VerifyFace::Objects (legacy MRT_GCV2_VERIFY_HEAP=1 is an alias).
// Report-only, every invocation,
// with the historical failure cap fixed at its default of 20 (HotSpot G1MaxVerifyFailures).
//
// Enumeration is independent of minor reachableObjects / TraceYoungClosure / remset.
// rootReachableHolders, when supplied, is a completed independent full-root closure.
// nullptr means the closure was not measured; an empty non-null set means it ran and
// found no holders. H3 uses a valid closure to split dead inventory from reachable edges.
void VerifyHeapObjects(const char* point,
                       const std::unordered_set<BaseObject*>* rootReachableHolders = nullptr);

} // namespace MapleRuntime

#endif // MRT_VERIFY_HEAP_H
