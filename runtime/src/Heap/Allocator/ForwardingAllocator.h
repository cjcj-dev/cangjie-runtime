// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


#pragma once


namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
inline bool ForwardingAllocator::contains_for_test(const void* address, size_t size) const {
        const uintptr_t start = reinterpret_cast<uintptr_t>(start_);
        const uintptr_t at = reinterpret_cast<uintptr_t>(address);
        return at >= start && at - start <= capacity_ && size <= capacity_ - (at - start);
    }
#endif
} // namespace MapleRuntime
