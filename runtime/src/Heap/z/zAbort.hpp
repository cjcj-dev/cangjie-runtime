// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0

#ifndef MRT_Z_ABORT_HPP
#define MRT_Z_ABORT_HPP

#include <atomic>

namespace MapleRuntime {

// ZGC zAbort.hpp:30-45 AllStatic + abortpoint()
class ZAbort {
public:
    static bool should_abort();
    static void abort();

private:
    static std::atomic<bool> _should_abort;
};

#define abortpoint()                  \
    do {                              \
        if (ZAbort::should_abort()) { \
            return;                   \
        }                             \
    } while (false)

} // namespace MapleRuntime

#endif // MRT_Z_ABORT_HPP
