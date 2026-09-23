// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0

#ifndef SHARE_GC_Z_ZDRIVERPORT_HPP
#define SHARE_GC_Z_ZDRIVERPORT_HPP

#include <cstdint>
#include <limits>

#include "Heap/z/zList.hpp"
#include "Heap/z/zLock.hpp"

namespace MapleRuntime {

enum GCReason : uint32_t {
    GC_REASON_USER = 0,
    GC_REASON_FORCE,
    GC_REASON_YOUNG,
    GC_REASON_WB_BREAKPOINT,
    GC_REASON_WARMUP,
    GC_REASON_ALLOCATION_STALL,
    GC_REASON_TIMER,
    GC_REASON_ALLOCATION_RATE,
    GC_REASON_HIGH_USAGE,
    GC_REASON_PROACTIVE,
    GC_REASON_DCMD_GC_RUN,
    GC_REASON_MAX,
    GC_REASON_INVALID = std::numeric_limits<uint32_t>::max(),
};

struct GCRequest {
    const GCReason reason;
    const char* name;
};

extern GCRequest g_gcRequests[GC_REASON_MAX];

class ZDriverPortEntry;

class ZDriverRequest {
private:
    GCReason _cause;
    uint32_t _young_nworkers;
    uint32_t _old_nworkers;

public:
    ZDriverRequest();
    ZDriverRequest(GCReason cause, uint32_t young_nworkers, uint32_t old_nworkers);

    bool operator==(const ZDriverRequest& other) const;

    GCReason cause() const;
    uint32_t young_nworkers() const;
    uint32_t old_nworkers() const;
};

class ZDriverPort {
private:
    mutable ZConditionLock _lock;
    bool _has_message;
    ZDriverRequest _message;
    uint64_t _seqnum;
    ZList<ZDriverPortEntry> _queue;

public:
    ZDriverPort();

    bool is_busy() const;

    void send_sync(const ZDriverRequest& request);
    void send_async(const ZDriverRequest& request);

    ZDriverRequest receive();
    void ack();
};

} // namespace MapleRuntime

#endif
