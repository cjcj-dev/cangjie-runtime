// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zUtils.cpp:24-41
#include "Heap/z/zUtils.hpp"

#include <pthread.h>

namespace MapleRuntime {
// zUtils.cpp:27-35. I17 (PLAN §5): GC threads are bare pthreads without a
// HotSpot Thread object, so the name is read back from the kernel.
const char* ZUtils::thread_name()
{
    static thread_local char name[32];
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) == 0 && name[0] != '\0') {
        return name;
    }
#endif
    return "Thread";
}

// zUtils.cpp:37-41
void ZUtils::fill(uintptr_t* addr, size_t count, uintptr_t value)
{
    for (size_t i = 0; i < count; ++i) {
        addr[i] = value;
    }
}
} // namespace MapleRuntime
