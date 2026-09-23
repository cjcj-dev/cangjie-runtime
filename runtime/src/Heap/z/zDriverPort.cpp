// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0

#include "Heap/z/zDriverPort.hpp"
#include "Base/Panic.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zFuture.inline.hpp"
#include "Heap/z/zList.inline.hpp"
#include "Heap/z/zLock.inline.hpp"

namespace MapleRuntime {

ZDriverRequest::ZDriverRequest()
    : ZDriverRequest(GC_REASON_INVALID, 0, 0)
{}

ZDriverRequest::ZDriverRequest(GCReason cause, uint32_t young_nworkers, uint32_t old_nworkers)
    : _cause(cause),
      _young_nworkers(young_nworkers),
      _old_nworkers(old_nworkers)
{}

bool ZDriverRequest::operator==(const ZDriverRequest& other) const
{
    return _cause == other._cause;
}

GCReason ZDriverRequest::cause() const
{
    return _cause;
}

uint32_t ZDriverRequest::young_nworkers() const
{
    return _young_nworkers;
}

uint32_t ZDriverRequest::old_nworkers() const
{
    return _old_nworkers;
}

class ZDriverPortEntry {
    friend class ZList<ZDriverPortEntry>;

private:
    const ZDriverRequest _message;
    uint64_t _seqnum;
    ZFuture<ZDriverRequest> _result;
    ZListNode<ZDriverPortEntry> _node;

public:
    explicit ZDriverPortEntry(const ZDriverRequest& message)
        : _message(message),
          _seqnum(0)
    {}

    void set_seqnum(uint64_t seqnum)
    {
        _seqnum = seqnum;
    }

    uint64_t seqnum() const
    {
        return _seqnum;
    }

    ZDriverRequest message() const
    {
        return _message;
    }

    void wait()
    {
        const ZDriverRequest message = _result.get();
        MRT_ASSERT(message == _message, "Message mismatch");
    }

    void satisfy(const ZDriverRequest& message)
    {
        _result.set(message);
    }
};

ZDriverPort::ZDriverPort()
    : _lock(),
      _has_message(false),
      _seqnum(0),
      _queue()
{}

bool ZDriverPort::is_busy() const
{
    ZLocker<ZConditionLock> locker(&_lock);
    return _has_message;
}

void ZDriverPort::send_sync(const ZDriverRequest& message)
{
    ZDriverPortEntry entry(message);

    {
        ZLocker<ZConditionLock> locker(&_lock);
        entry.set_seqnum(_seqnum);
        _queue.insert_last(&entry);
        _lock.notify();
    }

    entry.wait();

    {
        ZLocker<ZConditionLock> locker(&_lock);
    }
}

void ZDriverPort::send_async(const ZDriverRequest& message)
{
    ZLocker<ZConditionLock> locker(&_lock);
    if (!_has_message) {
        _message = message;
        _has_message = true;
        _lock.notify();
    }
}

ZDriverRequest ZDriverPort::receive()
{
    ZLocker<ZConditionLock> locker(&_lock);

    while (!_has_message && _queue.is_empty()) {
        _lock.wait();
    }

    _seqnum++;

    if (!_has_message) {
        _message = _queue.first()->message();
        _has_message = true;
    }

    return _message;
}

void ZDriverPort::ack()
{
    ZLocker<ZConditionLock> locker(&_lock);

    if (!_has_message) {
        return;
    }

    ZListIterator<ZDriverPortEntry> iter(&_queue);
    for (ZDriverPortEntry* entry; iter.next(&entry);) {
        if (entry->message() == _message && entry->seqnum() < _seqnum) {
            _queue.remove(entry);
            entry->satisfy(_message);
        }
    }

    if (_queue.is_empty()) {
        _has_message = false;
    } else {
        _message = _queue.first()->message();
    }
}

inline bool GCRequest::IsFrequentGC() const
{
    uint64_t intervalNs = minIntervelNs.load(std::memory_order_acquire);
    if (intervalNs == 0) {
        return false;
    }
    int64_t now = static_cast<int64_t>(TimeUtil::NanoSeconds());
    return (now - static_cast<int64_t>(prevRequestTime.load(std::memory_order_acquire)) <
            static_cast<int64_t>(intervalNs));
}

inline bool GCRequest::IsFrequentAsyncGC() const
{
    int64_t now = static_cast<int64_t>(TimeUtil::NanoSeconds());
    return (now - ZStat::GetPrevGCFinishTime() < minIntervelNs.load(std::memory_order_acquire));
}

inline bool GCRequest::IsFrequentHeuristicGC() const { return IsFrequentAsyncGC(); }

bool GCRequest::ShouldBeIgnored() const
{
    switch (reason) {
        case GC_REASON_YOUNG:
            return false;
        case GC_REASON_FORCE:
            return IsFrequentGC();
        default:
            return false;
    }
}

GCRequest g_gcRequests[] = {
    { GC_REASON_USER, "user", false, true, { 0 }, { 0 } },
    { GC_REASON_FORCE, "force", true, false, { 0 }, { 0 } },
    { GC_REASON_YOUNG, "young", false, false, { 0 }, { 0 } },
    { GC_REASON_WB_BREAKPOINT, "wb_breakpoint", false, true, { 0 }, { 0 } },
    { GC_REASON_WARMUP, "warmup", false, true, { 0 }, { 0 } },
    { GC_REASON_ALLOCATION_STALL, "allocation_stall", false, false, { 0 }, { 0 } },
    { GC_REASON_TIMER, "timer", false, true, { 0 }, { 0 } },
    { GC_REASON_ALLOCATION_RATE, "allocation_rate", false, true, { 0 }, { 0 } },
    { GC_REASON_HIGH_USAGE, "high_usage", false, true, { 0 }, { 0 } },
    { GC_REASON_PROACTIVE, "proactive", false, true, { 0 }, { 0 } },
    { GC_REASON_DCMD_GC_RUN, "diagnostic_command", true, true, { 0 }, { 0 } }
};

} // namespace MapleRuntime
