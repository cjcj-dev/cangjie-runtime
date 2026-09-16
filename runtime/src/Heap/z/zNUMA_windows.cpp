// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/windows/gc/z/zNUMA_windows.cpp:29-51. NUMA disabled; count=1 (A03n).

#include "Heap/z/zNUMA.hpp"

namespace MapleRuntime {

void NumaTopology::numa_make_local(void* addr, size_t size, uint32_t numa_id)
{
    (void)addr;
    (void)size;
    (void)numa_id;
}

NumaTopology NumaTopology::SealProcessTopology()
{
    return Seal({});
}

}
