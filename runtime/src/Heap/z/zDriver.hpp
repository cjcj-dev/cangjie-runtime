// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COLLECTOR_RESOURCES_H
#define MRT_COLLECTOR_RESOURCES_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>

#include "Base/Macros.h"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zReferenceProcessor.hpp"
#include <condition_variable>
#include <cstdint>
#include <list>
#include "Base/Panic.h"
#include "Common/PageAllocator.h"
#include "Heap/z/zHeap.hpp"
#include "Inspector/HeapSnapshotJsonSerializer.h"
#include "Heap/z/zThread.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Inspector/CjHeapData.h"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zDriverPort.hpp"
#include "Heap/z/zLock.hpp"
#include "Heap/z/zResurrection.inline.hpp"

// ZGC zDriver.hpp:32-41: system headers may define these names as macros.
#ifdef minor
#undef minor
#endif
#ifdef major
#undef major
#endif

namespace MapleRuntime {

// zDriver.hpp:48-119: ZDriverMinor/ZDriverMajor are ZThreads whose run_thread
// receives requests from their port and whose terminate closes that port.
class ZDriverMinor;
class ZDriverMajor;

class ZDriver : public ZThread {
public:
    static void initialize();
    static void set_minor(ZDriverMinor* minor);
    static void set_major(ZDriverMajor* major);
    static ZDriverMinor* minor();
    static ZDriverMajor* major();
    static void lock();
    static void unlock();
    ZDriver();
    void set_gc_cause(GCReason cause);
    GCReason gc_cause() const;
private:
    GCReason _gc_cause;
    static ZLock* _lock;
    static ZDriverMinor* _minor;
    static ZDriverMajor* _major;
};

class ZDriverMinor final : public ZDriver {
public:
    ZDriverMinor();
    bool is_busy() const;
    void collect(const ZDriverRequest& request);
    ZDriverPort& port() { return _port; }
    const ZDriverPort& port() const { return _port; }
private:
    void run_thread() override;
    void terminate() override;
    void handle_alloc_stalls() const;
    void gc(const ZDriverRequest& request);
    ZDriverPort _port;
};

class ZDriverMajor final : public ZDriver {
public:
    ZDriverMajor();
    bool is_busy() const;
    void collect(const ZDriverRequest& request);
    ZDriverPort& port() { return _port; }
    const ZDriverPort& port() const { return _port; }
private:
    void run_thread() override;
    void terminate() override;
    void handle_alloc_stalls() const;
    void collect_young(const ZDriverRequest& request);
    void collect_old();
    void gc(const ZDriverRequest& request);
    ZDriverPort _port;
};

// zDriver.cpp:85-107: lock scopes shared by both generation drivers.
class DriverLocker {
public:
    DriverLocker() { ZDriver::lock(); }
    ~DriverLocker() { ZDriver::unlock(); }
    DriverLocker(const DriverLocker&) = delete;
    DriverLocker& operator=(const DriverLocker&) = delete;
};

class DriverUnlocker {
public:
    DriverUnlocker() { ZDriver::unlock(); }
    ~DriverUnlocker() { ZDriver::lock(); }
    DriverUnlocker(const DriverUnlocker&) = delete;
    DriverUnlocker& operator=(const DriverUnlocker&) = delete;
};
} // namespace MapleRuntime
#endif // MRT_COLLECTOR_RESOURCES_H
