// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_SHARED_CONCURRENTGCTHREAD_HPP
#define MRT_GC_SHARED_CONCURRENTGCTHREAD_HPP

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <pthread.h>

namespace MapleRuntime {
// gc/shared/concurrentGCThread.hpp:33-63. The NamedThread/Thread layers of
// HotSpot do not exist here (I17): the OS thread handle and name live in
// this class, and Terminator_lock is a per-thread monitor (I14).
class ConcurrentGCThread {
private:
    std::atomic<bool> _should_terminate;
    std::atomic<bool> _has_terminated;
    pthread_t _thread;
    char _name[32];

    static void* entry(void* arg);

protected:
    std::mutex _terminator_lock;
    std::condition_variable _terminator_condition;

    void create_and_start();

    virtual void run_service() = 0;
    virtual void stop_service() = 0;

public:
    ConcurrentGCThread();
    virtual ~ConcurrentGCThread() = default;
    ConcurrentGCThread(const ConcurrentGCThread&) = delete;
    ConcurrentGCThread& operator=(const ConcurrentGCThread&) = delete;

    virtual bool is_ConcurrentGC_thread() const { return true; }

    // runtime/init.cpp:245-258: publish only after runtime initialization.
    static void NotifyRuntimeInitialized();
    static bool IsRuntimeInitialized();

    virtual void run();
    virtual void stop();

    bool should_terminate() const;
    bool has_terminated() const;

    void set_name(const char* name);
    const char* name() const { return _name; }
    pthread_t os_thread() const { return _thread; }

    // Printing
    const char* type_name() const { return "ConcurrentGCThread"; }
};
} // namespace MapleRuntime
#endif // MRT_GC_SHARED_CONCURRENTGCTHREAD_HPP
