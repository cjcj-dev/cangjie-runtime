// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0

#ifndef MRT_Z_ABORT_HPP
#define MRT_Z_ABORT_HPP

#include <atomic>

namespace MapleRuntime {

// Cooperative cancellation token modelled after ZGC's ZAbort (zAbort.hpp:30-45).
// A driver owns one token for its cycle; polling is deliberately cheap and safe
// from worker code, while Request() is used by the peer driver or shutdown path.
class ZAbort {
public:
    void Request() { requested.store(true, std::memory_order_release); }
    void Reset() { requested.store(false, std::memory_order_release); }
    bool IsRequested() const { return requested.load(std::memory_order_acquire); }
    bool Poll() const { return IsRequested(); }

private:
    std::atomic<bool> requested { false };
};

} // namespace MapleRuntime

#endif // MRT_Z_ABORT_HPP
