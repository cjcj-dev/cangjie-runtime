// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0

#ifndef MRT_GC_DRIVER_PORT_H
#define MRT_GC_DRIVER_PORT_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

#include "GcRequest.h"
#include "ZAbort.hpp"

namespace MapleRuntime {

enum class GCDriverKind : uint8_t { MINOR, MAJOR };

struct GCDriverReceiptState;

// A receipt identifies one accepted synchronous request. Sequence numbers are
// diagnostic only: completion is carried by the state object, so wrap-around
// and unrelated acknowledgements cannot satisfy this receipt.
class GCDriverReceipt {
public:
    GCDriverReceipt() = default;

    uint64_t Sequence() const;
    bool IsValid() const;

private:
    friend class GCDriverPort;
    explicit GCDriverReceipt(std::shared_ptr<GCDriverReceiptState> state) : state(std::move(state)) {}

    std::shared_ptr<GCDriverReceiptState> state;
};

struct GCDriverRequest {
    uint64_t sequence;
    GCReason reason;
    bool asynchronous;
    GCDriverReceipt receipt;
};

// Per-generation request port.  Keeping the queues separate is the important
// invariant: an old/full request cannot erase a young request as it could in a
// single priority bitmap (the ZGC zDriverPort sync/async contract).
class GCDriverPort {
public:
    explicit GCDriverPort(GCDriverKind kind) : kind(kind) {}

    GCDriverReceipt EnqueueSync(GCReason reason);
    uint64_t EnqueueAsync(GCReason reason);
    bool TryDequeue(GCDriverRequest& request);
    void Acknowledge(const GCDriverRequest& request);
    void Cancel(const GCDriverRequest& request);
    bool WaitForAck(const GCDriverReceipt& receipt);
    void Stop();
    void Reset();
    bool IsStopped() const;
    size_t Pending() const;
    GCDriverKind Kind() const { return kind; }
    ZAbort& Abort() { return abort; }
    const ZAbort& Abort() const { return abort; }

private:
#if defined(MRT_GC_UNIT_TESTS)
    friend class GCDriverPortTestPeer;
#endif
    uint64_t NextSequenceLocked();

    const GCDriverKind kind;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<GCDriverRequest> requests;
    uint64_t nextSequence { 2 };
    // Diagnostic high-water mark only. Receipt completion must never be
    // derived from this value because sequence numbers wrap.
    uint64_t highestAcknowledged { 1 };
#if defined(MRT_GC_UNIT_TESTS)
    size_t waitingReceipts { 0 };
#endif
    bool stopped { false };
    ZAbort abort;
};

} // namespace MapleRuntime

#endif // MRT_GC_DRIVER_PORT_H
