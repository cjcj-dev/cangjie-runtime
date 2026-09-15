// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/shared/workerThread.cpp:211
#include "Heap/z/workerThread.hpp"

#include <climits>

namespace MapleRuntime {
thread_local uint32_t WorkerThread::_worker_id = UINT32_MAX;
} // namespace MapleRuntime
