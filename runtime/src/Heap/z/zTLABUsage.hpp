// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#pragma once

#include <atomic>
#include "Base/TruncatedSeq.h"

namespace MapleRuntime {
// ZGC zTLABUsage.hpp:43-59: live accounting and snapshotted cycle history.
class ZTLABUsage {
public:
    ZTLABUsage();
    void increase_used(size_t size);
    void decrease_used(size_t size);
    void reset();
    size_t tlab_used() const;
    size_t tlab_capacity() const;
private:
    std::atomic<size_t> _used;
    TruncatedSeq _used_history;
};
}
