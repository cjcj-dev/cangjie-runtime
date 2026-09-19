// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COLLECTOR_H
#define MRT_COLLECTOR_H

#include "Heap/z/zInitialize.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRuntimeWorkers.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zReferenceProcessor.hpp"
#include <atomic>

namespace MapleRuntime {
enum class Generation : uint8_t;
class ZDirector;
class ZDriverMajor;
class ZDriverMinor;
class ZStat;

class ZCollectedHeap {
public:
    static ZCollectedHeap* heap();
    static void create(const HeapParam& param, double garbageThreshold);
    ZCollectedHeap(const HeapParam& param, double garbageThreshold);
    ~ZCollectedHeap();
    static void stop();
    void initialize_gc_workers();
    void initialize_gc();
    void finalize_gc();
    void collect(GCReason reason, bool async);

    Heap& collected_heap() { return _heap; }
    const Heap& collected_heap() const { return _heap; }
    ZDriverMinor* driver_minor() const { return _driver_minor; }
    ZDriverMajor* driver_major() const { return _driver_major; }
    ZDirector* director() const { return _director; }
    ZStat* stat() const { return _stat; }
    WorkerThreads* safepoint_workers() { return _runtime_workers.workers(); }
    FinalizerProcessor& finalizer_processor() { return _finalizer_processor; }
    int32_t concurrent_gc_threads() const { return _concurrent_gc_threads; }
#if defined(MRT_TESTABLE_INTERNALS)
    void set_concurrent_gc_threads_for_test(int32_t count) { _concurrent_gc_threads = count; }
#endif

private:
    static ZCollectedHeap* _collected_heap;
    ZInitializer _initializer;
    Heap _heap;
    ZDriverMinor* _driver_minor;
    ZDriverMajor* _driver_major;
    ZDirector* _director;
    ZStat* _stat;
    ZRuntimeWorkers _runtime_workers;
    FinalizerProcessor _finalizer_processor;
    int32_t _concurrent_gc_threads = 1;
    std::atomic<bool> _gc_thread_running { true };
};
} // namespace MapleRuntime

#endif
