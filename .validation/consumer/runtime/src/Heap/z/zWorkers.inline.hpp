// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_Z_ZWORKERS_INLINE_HPP
#define MRT_GC_Z_ZWORKERS_INLINE_HPP

#include "Heap/z/zWorkers.hpp"

namespace MapleRuntime {
// zWorkers.inline.hpp:31-33: a lock-free read on the worker hot path.
inline bool ZWorkers::should_worker_resize()
{
    return _requested_nworkers.load(std::memory_order_relaxed) != 0;
}
} // namespace MapleRuntime
#endif // MRT_GC_Z_ZWORKERS_INLINE_HPP
