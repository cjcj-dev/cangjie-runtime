// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0

#ifndef SHARE_GC_Z_ZDRIVERPORT_HPP
#define SHARE_GC_Z_ZDRIVERPORT_HPP

#include "Heap/Collector/GcRequest.h"
#include "Heap/z/zList.hpp"
#include "Heap/z/zLock.hpp"

namespace MapleRuntime {

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
