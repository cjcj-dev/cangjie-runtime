// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


#pragma once


namespace MapleRuntime {
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
inline size_t AllocationStallQueue::Pending() const {
        std::lock_guard<std::mutex> lock(mutex);
        return requests.size();
    }
#endif
} // namespace MapleRuntime

namespace MapleRuntime {
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
inline size_t AllocationStallQueue::EnqueuedCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return enqueued;
    }
#endif
} // namespace MapleRuntime

namespace MapleRuntime {
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
inline size_t AllocationStallQueue::DequeuedCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return dequeued;
    }
#endif
} // namespace MapleRuntime

namespace MapleRuntime {
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
inline size_t AllocationStallQueue::SatisfiedCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return satisfiedCount;
    }
#endif
} // namespace MapleRuntime

namespace MapleRuntime {
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
inline size_t AllocationStallQueue::FailedCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return failedCount;
    }
#endif
} // namespace MapleRuntime
