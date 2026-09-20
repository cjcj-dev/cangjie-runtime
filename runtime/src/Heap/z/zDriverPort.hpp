// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0

#ifndef SHARE_GC_Z_ZDRIVERPORT_HPP
#define SHARE_GC_Z_ZDRIVERPORT_HPP

#include <atomic>
#include <cstdint>
#include <limits>

#include "Base/Globals.h"
#include "Heap/z/zList.hpp"
#include "Heap/z/zLock.hpp"

namespace MapleRuntime {

constexpr uint64_t MIN_ASYNC_GC_INTERVAL_NS = MapleRuntime::SECOND_TO_NANO_SECOND;
constexpr uint64_t LONG_MIN_HEU_GC_INTERVAL_NS = 200 * MapleRuntime::MILLI_SECOND_TO_NANO_SECOND;

enum GCReason : uint32_t {
    GC_REASON_USER = 0,
    GC_REASON_OOM,
    GC_REASON_BACKUP,
    GC_REASON_HEU,
    GC_REASON_NATIVE,
    GC_REASON_HEU_SYNC,
    GC_REASON_NATIVE_SYNC,
    GC_REASON_FORCE,
    GC_REASON_YOUNG,
    GC_REASON_WB_BREAKPOINT,
    GC_REASON_WARMUP,
    GC_REASON_ALLOCATION_STALL,
    GC_REASON_MAX,
    GC_REASON_INVALID = std::numeric_limits<uint32_t>::max(),
};

struct GCRequest {
    const GCReason reason;
    const char* name;
    const bool isSync;
    const bool isConcurrent;
    std::atomic<uint64_t> minIntervelNs;
    std::atomic<uint64_t> prevRequestTime;
    inline bool IsFrequentGC() const;
    inline bool IsFrequentAsyncGC() const;
    inline bool IsFrequentHeuristicGC() const;
    bool ShouldBeIgnored() const;
    bool IsSyncGC() const { return isSync; }
    void SetMinInterval(const uint64_t intervalNs)
    {
        minIntervelNs.store(intervalNs, std::memory_order_release);
    }
    uint64_t GetMinInterval() const
    {
        return minIntervelNs.load(std::memory_order_acquire);
    }
    void SetPrevRequestTime(uint64_t timestamp)
    {
        prevRequestTime.store(timestamp, std::memory_order_release);
    }
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
