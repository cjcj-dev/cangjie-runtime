// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_VERIFY_HEAP_H
#define MRT_VERIFY_HEAP_H

#include <cstddef>

namespace MapleRuntime {

// Full-heap object verifier — HotSpot G1HeapVerifier::verify inventory #10.
//
// Invariant H (per live object o walked via Heap::ForEachObj):
//   H1  o->IsValidObject()
//   H2  tip = o->GetTypeInfo() is not a heap address; preferably in TypeInfoManager
//       mmap OR other non-heap; and tip->IsVaildType()
//   H3  each ref field is null or points at an object that itself satisfies H1+H2
//   H4  holder region is not free/garbage
//
// Gate (default off):  MRT_GCV2_VERIFY_HEAP=1
// Cost control:        MRT_GCV2_VERIFY_HEAP_START_AT=<N>   (1-based invoke count)
//                      MRT_GCV2_VERIFY_HEAP_EVERY=<N>
// Failure cap:         MRT_GCV2_VERIFY_HEAP_MAX_FAILURES=<N>  (default 20; HotSpot G1MaxVerifyFailures)
// Report-only; optional abort: MRT_GCV2_VERIFY_HEAP_FATAL=1
//
// Enumeration is independent of minor reachableObjects / TraceYoungClosure / remset.
void VerifyHeapObjects(const char* point);

} // namespace MapleRuntime

#endif // MRT_VERIFY_HEAP_H
