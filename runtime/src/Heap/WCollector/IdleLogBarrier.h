// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_IDLE_LOG_BARRIER_H
#define MRT_IDLE_LOG_BARRIER_H

#include "IdleBarrier.h"

namespace MapleRuntime {
class IdleLogBarrier : public IdleBarrier {
public:
    explicit IdleLogBarrier(Collector& collector) : IdleBarrier(collector) {}
};
} // namespace MapleRuntime
#endif // MRT_IDLE_LOG_BARRIER_H
