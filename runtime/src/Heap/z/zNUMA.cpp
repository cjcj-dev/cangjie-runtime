// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zVirtualMemoryManager.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <cerrno>
#elif !defined(_WIN64)
#include <sys/resource.h>
#endif
#ifdef _WIN64
#include <errhandlingapi.h>
#include <handleapi.h>
#include <memoryapi.h>
#include <sysinfoapi.h>
#endif

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/Panic.h"
#include "Base/SysCall.h"

#include "Heap/z/zVirtualMemory.inline.hpp"

namespace MapleRuntime {
NumaTopology NumaTopology::Seal(const std::vector<uint32_t>& nodeIds)
{
    NumaTopology topology;
    topology.nodes = nodeIds;
    std::sort(topology.nodes.begin(), topology.nodes.end());
    topology.nodes.erase(std::unique(topology.nodes.begin(), topology.nodes.end()), topology.nodes.end());
    if (topology.nodes.empty()) {
        topology.nodes.push_back(0);
    }
    topology.sealed = true;
    return topology;
}

NumaTopology NumaTopology::SealProcessTopology()
{
    std::vector<uint32_t> nodes;
#if defined(__linux__) && defined(SYS_get_mempolicy)
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

bool NumaTopology::Contains(uint32_t node) const
{
    return std::binary_search(nodes.begin(), nodes.end(), node);
}

}

#include "Heap/z/zNUMA.inline.hpp"
