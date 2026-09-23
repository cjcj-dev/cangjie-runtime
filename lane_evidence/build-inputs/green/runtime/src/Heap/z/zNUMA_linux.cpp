// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/linux/gc/z/zNUMA_linux.cpp. A03n owns the static ZNUMA class;
// this file only probes process mempolicy for NumaTopology.

#include "Heap/z/zNUMA.hpp"

#include <sys/syscall.h>
#include <unistd.h>

namespace MapleRuntime {
namespace {
constexpr unsigned long kMaxNumaNodes = sizeof(unsigned long) * 8;
constexpr int kMpolMemsAllowed = 2;
}

void NumaTopology::numa_make_local(void* addr, size_t size, uint32_t numa_id)
{
    constexpr int kMpolBind = 2;
    constexpr int kMpolMfMove = 1;
    const uint32_t node = SealProcessTopology().NodeAt(numa_id);
    unsigned long mask = node < kMaxNumaNodes ? (1UL << node) : 0UL;
#ifdef SYS_mbind
    (void)syscall(SYS_mbind, addr, size, kMpolBind, &mask, kMaxNumaNodes, kMpolMfMove);
#else
    (void)addr;
    (void)size;
    (void)mask;
#endif
}

NumaTopology NumaTopology::SealProcessTopology()
{
    std::vector<uint32_t> nodes;
#ifdef SYS_get_mempolicy
    unsigned long mask = 0;
    const long rc = syscall(SYS_get_mempolicy, nullptr, &mask, kMaxNumaNodes, nullptr, kMpolMemsAllowed);
    if (rc == 0) {
        for (uint32_t node = 0; node < kMaxNumaNodes; ++node) {
            if ((mask & (1UL << node)) != 0) {
                nodes.push_back(node);
            }
        }
    }
#endif
    return Seal(nodes);
}

}
