// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COLLECTOR_H
#define MRT_COLLECTOR_H

#include "Heap/z/zHeap.hpp"
#include "Heap/z/zForwardingLookup.hpp"

namespace MapleRuntime {
enum class Generation : uint8_t;
class HeapGcState;
class CollectorResources;
class ZDirector;
class ZDriverMajor;
class ZDriverMinor;
class ZStat;

class ZRuntimeWorkers {
public:
    ZRuntimeWorkers() = default;
};

class ZCollectedHeap {
public:
    static ZCollectedHeap* heap();
    ZCollectedHeap();
    ~ZCollectedHeap();
    static void stop();
    void start_gc_threads();
    void initialize_gc();
    void finalize_gc();
    void collect(GCReason reason, bool async);
    CollectorResources& resources() { return *_resources; }
    const CollectorResources& resources() const { return *_resources; }

    Heap& collected_heap() { return _heap; }
    const Heap& collected_heap() const { return _heap; }
    ZDriverMinor* driver_minor() const { return _driver_minor; }
    ZDriverMajor* driver_major() const { return _driver_major; }
    ZDirector* director() const { return _director; }
    ZStat* stat() const { return _stat; }

    friend class CollectorResources;

private:
    Heap _heap;
    ZDriverMinor* _driver_minor;
    ZDriverMajor* _driver_major;
    ZDirector* _director;
    ZStat* _stat;
    ZRuntimeWorkers _runtime_workers;
    CollectorResources* _resources;
};
} // namespace MapleRuntime

#endif
