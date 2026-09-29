// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once

#include <atomic>
#include "Base/Semaphore.h"

namespace MapleRuntime {
// HotSpot utilities/singleWriterSynchronizer.hpp:51-99. Readers select
// an exit counter by the low bit; a serialized writer changes that bit.
class SingleWriterSynchronizer {
public:
    unsigned Enter() { return enter.fetch_add(2u) + 2u; }
    void Exit(unsigned entered)
    {
        const unsigned exited = exits[entered & 1u].fetch_add(2u) + 2u;
        if (exited == waitingFor.load(std::memory_order_relaxed)) {
            wakeup.signal();
        }
    }
    void Synchronize();

    SingleWriterSynchronizer() = default;
    SingleWriterSynchronizer(const SingleWriterSynchronizer&) = delete;
    SingleWriterSynchronizer& operator=(const SingleWriterSynchronizer&) = delete;
private:
    std::atomic<unsigned> enter{0};
    std::atomic<unsigned> exits[2]{{0}, {0}};
    std::atomic<unsigned> waitingFor{1};
    Semaphore wakeup;
};
} // namespace MapleRuntime
