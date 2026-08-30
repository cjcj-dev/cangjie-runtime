// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0

#include "DriverPort.h"

#include <limits>

namespace MapleRuntime {

struct GCDriverReceiptState {
    explicit GCDriverReceiptState(uint64_t sequence) : sequence(sequence) {}

    const uint64_t sequence;
    bool resolved { false };
    bool completed { false };
};

uint64_t GCDriverReceipt::Sequence() const
{
    return state == nullptr ? 0 : state->sequence;
}

bool GCDriverReceipt::IsValid() const
{
    return state != nullptr;
}

uint64_t GCDriverPort::NextSequenceLocked()
{
    const uint64_t sequence = nextSequence;
    ++nextSequence;
    if (nextSequence == 0 || nextSequence == std::numeric_limits<uint64_t>::max()) {
        nextSequence = 2;
    }
    return sequence;
}

GCDriverReceipt GCDriverPort::EnqueueSync(GCReason reason)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (stopped) {
        return {};
    }
    for (auto& pending : requests) {
        if (pending.reason == reason) {
            if (!pending.receipt.IsValid()) {
                pending.receipt = GCDriverReceipt(std::make_shared<GCDriverReceiptState>(pending.sequence));
            }
            return pending.receipt;
        }
    }
    const uint64_t sequence = NextSequenceLocked();
    GCDriverReceipt receipt(std::make_shared<GCDriverReceiptState>(sequence));
    requests.push_back({ sequence, reason, false, receipt });
    condition.notify_all();
    return receipt;
}

uint64_t GCDriverPort::EnqueueAsync(GCReason reason)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (stopped) {
        return 0;
    }
    // Async requests are intentionally deduplicated only within this port.
    for (const auto& pending : requests) {
        if (pending.reason == reason) {
            return pending.sequence;
        }
    }
    const uint64_t sequence = NextSequenceLocked();
    requests.push_back({ sequence, reason, true, {} });
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

void GCDriverPort::Acknowledge(const GCDriverRequest& request)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (request.receipt.state != nullptr) {
        request.receipt.state->completed = true;
        request.receipt.state->resolved = true;
    }
    if (request.sequence > highestAcknowledged) {
        highestAcknowledged = request.sequence;
    }
    condition.notify_all();
}

void GCDriverPort::Cancel(const GCDriverRequest& request)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (request.receipt.state != nullptr) {
        request.receipt.state->resolved = true;
    }
    condition.notify_all();
}

bool GCDriverPort::WaitForAck(const GCDriverReceipt& receipt)
{
    std::unique_lock<std::mutex> lock(mutex);
    if (receipt.state == nullptr) {
        return false;
    }
#if defined(MRT_GC_UNIT_TESTS)
    ++waitingReceipts;
    condition.notify_all();
#endif
    condition.wait(lock, [this, &receipt] { return stopped || receipt.state->resolved; });
#if defined(MRT_GC_UNIT_TESTS)
    --waitingReceipts;
    condition.notify_all();
#endif
    return receipt.state->completed;
}

void GCDriverPort::Stop()
{
    std::lock_guard<std::mutex> lock(mutex);
    stopped = true;
    abort.Request();
    for (auto& request : requests) {
        if (request.receipt.state != nullptr) {
            request.receipt.state->resolved = true;
        }
    }
    condition.notify_all();
}

void GCDriverPort::Reset()
{
    std::lock_guard<std::mutex> lock(mutex);
    requests.clear();
    stopped = false;
    nextSequence = 2;
    highestAcknowledged = 1;
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
