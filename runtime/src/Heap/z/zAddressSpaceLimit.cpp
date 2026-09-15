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
AddressSpaceBudget AddressSpaceBudget::Seal(size_t available, size_t safeFraction)
{
    AddressSpaceBudget budget;
    if (safeFraction == 0) {
        return budget;
    }
    budget.availableBytes = available;
    budget.safeBytes = available / safeFraction;
    budget.sealed = true;
    return budget;
}

AddressSpaceBudget AddressSpaceBudget::SealProcessBudget()
{
#ifdef _WIN64
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) {
        return Seal(0, kDefaultSafeFraction);
    }
    return Seal(static_cast<size_t>(status.ullTotalVirtual), kDefaultSafeFraction);
#else
    struct rlimit limit {};
    if (getrlimit(RLIMIT_AS, &limit) != 0) {
        return Seal(0, kDefaultSafeFraction);
    }
    const size_t available = limit.rlim_cur == RLIM_INFINITY ? std::numeric_limits<size_t>::max()
                                                             : static_cast<size_t>(limit.rlim_cur);
    return Seal(available, kDefaultSafeFraction);
#endif
}

}
