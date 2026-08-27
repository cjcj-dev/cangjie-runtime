// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0

#ifndef MRT_GC_DRIVER_PORT_H
#define MRT_GC_DRIVER_PORT_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "GcRequest.h"
#include "ZAbort.hpp"

namespace MapleRuntime {

enum class GCDriverKind : uint8_t { MINOR, MAJOR };

struct GCDriverRequest {
    uint64_t sequence;
    GCReason reason;
    bool asynchronous;
};

// Per-generation request port.  Keeping the queues separate is the important
// invariant: an old/full request cannot erase a young request as it could in a
// single priority bitmap (the ZGC zDriverPort sync/async contract).
class GCDriverPort {
public:
    explicit GCDriverPort(GCDriverKind kind) : kind(kind) {}

    uint64_t EnqueueSync(GCReason reason);
    uint64_t EnqueueAsync(GCReason reason);
    bool TryDequeue(GCDriverRequest& request);
    void Acknowledge(uint64_t sequence);
    bool WaitForAck(uint64_t sequence);
    void Stop();
    void Reset();
    bool IsStopped() const;
    size_t Pending() const;
    GCDriverKind Kind() const { return kind; }
    ZAbort& Abort() { return abort; }
    const ZAbort& Abort() const { return abort; }

private:
    uint64_t NextSequenceLocked();

    const GCDriverKind kind;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<GCDriverRequest> requests;
    uint64_t nextSequence { 2 };
    uint64_t acknowledged { 1 };
    bool stopped { false };
    ZAbort abort;
};

} // namespace MapleRuntime

#endif // MRT_GC_DRIVER_PORT_H
