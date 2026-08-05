// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_B3_ROOT_H
#define MRT_HEAP_B3_ROOT_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

// Dual-mode root-holder classify for F3 bucket-1 (valid unmarked holder).
// Mode A = ENUM six families (same as InvalidateOldTaggedRefs / major root enum).
// Mode B = wider: conservative mutator stack + static roots + concurrency (TLS proxy).
//
// Gate (default off): MRT_GCV2_B3ROOT=1
// Logs [GCV2][B3ROOT].
class B3Root {
public:
    static bool Enabled();

    // holder = F3 abort holder object. collector provides finalizer access.
    // fieldAddr / loadFromHeapField for T2 (mutator read site).
    static void ClassifyHolder(void* holder, int holderValid, int holderMarked, void* fieldAddr,
                               int loadFromHeapField, void* collectorOpaque);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_B3_ROOT_H
