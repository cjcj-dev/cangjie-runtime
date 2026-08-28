// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0

#include "DriverPort.h"

#include <limits>

namespace MapleRuntime {

uint64_t GCDriverPort::NextSequenceLocked()
{
    const uint64_t sequence = nextSequence;
    ++nextSequence;
    if (nextSequence == 0 || nextSequence == std::numeric_limits<uint64_t>::max()) {
        nextSequence = 2;
    }
    return sequence;
}

uint64_t GCDriverPort::EnqueueSync(GCReason reason)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (stopped) {
        return acknowledged;
    }
    for (const auto& pending : requests) {
        if (pending.reason == reason) {
            return pending.sequence;
        }
    }
    const uint64_t sequence = NextSequenceLocked();
    requests.push_back({ sequence, reason, false });
    condition.notify_all();
    return sequence;
}

uint64_t GCDriverPort::EnqueueAsync(GCReason reason)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (stopped) {
        return acknowledged;
    }
    // Async requests are intentionally deduplicated only within this port.
    for (const auto& pending : requests) {
        if (pending.reason == reason) {
            return pending.sequence;
        }
    }
    const uint64_t sequence = NextSequenceLocked();
    requests.push_back({ sequence, reason, true });
    condition.notify_all();
    return sequence;
}

bool GCDriverPort::TryDequeue(GCDriverRequest& request)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (requests.empty()) {
        return false;
    }
    request = requests.front();
    requests.pop_front();
    return true;
}

void GCDriverPort::Acknowledge(uint64_t sequence)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (sequence > acknowledged) {
        acknowledged = sequence;
    }
    condition.notify_all();
}

bool GCDriverPort::WaitForAck(uint64_t sequence)
{
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [this, sequence] { return stopped || acknowledged >= sequence; });
    return acknowledged >= sequence;
}

void GCDriverPort::Stop()
{
    std::lock_guard<std::mutex> lock(mutex);
    stopped = true;
    abort.Request();
    condition.notify_all();
}

void GCDriverPort::Reset()
{
    std::lock_guard<std::mutex> lock(mutex);
    requests.clear();
    stopped = false;
    nextSequence = 2;
    acknowledged = 1;
    abort.Reset();
}

bool GCDriverPort::IsStopped() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return stopped;
}

size_t GCDriverPort::Pending() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return requests.size();
}

} // namespace MapleRuntime
