// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#include "Heap/z/zForwardingAllocator.hpp"

namespace MapleRuntime {
ForwardingAllocator::ForwardingAllocator(size_t capacity)
    : start_(capacity == 0 ? nullptr : std::malloc(capacity)), capacity_(capacity), top_(0)
{}
}

namespace MapleRuntime {
ForwardingAllocator::~ForwardingAllocator()
{ std::free(start_); }
}
