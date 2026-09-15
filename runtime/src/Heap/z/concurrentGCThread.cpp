// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/concurrentGCThread.hpp"

#include <cerrno>
#include <cstring>
#if defined(__linux__) || defined(hongmeng)
#include <sys/prctl.h>
#endif

#include "Base/Log.h"
#include "Mutator/ThreadLocal.h"

namespace MapleRuntime {
// gc/shared/concurrentGCThread.cpp:33-35
ConcurrentGCThread::ConcurrentGCThread() : _should_terminate(false), _has_terminated(false), _thread()
{
    _name[0] = '\0';
}

void ConcurrentGCThread::set_name(const char* name)
{
    (void)std::strncpy(_name, name, sizeof(_name) - 1);
    _name[sizeof(_name) - 1] = '\0';
}

// gc/shared/concurrentGCThread.cpp:37-41: os::create_thread(this, os::gc_thread)
// + os::start_thread(this). Thread::call_run's NamedThread setup is the
// thread-type registration and naming in entry().
void ConcurrentGCThread::create_and_start()
{
    // A HotSpot ConcurrentGCThread is created once; this runtime's
    // Uncommitter is restarted by its owner, so the flags reset here.
    _should_terminate.store(false, std::memory_order_relaxed);
    _has_terminated.store(false, std::memory_order_relaxed);
    CHECK_PTHREAD_CALL(pthread_create, (&_thread, nullptr, ConcurrentGCThread::entry, this), "ConcurrentGCThread");
}

void* ConcurrentGCThread::entry(void* arg)
{
    ConcurrentGCThread* const thread = static_cast<ConcurrentGCThread*>(arg);
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
#ifdef __APPLE__
    CHECK_PTHREAD_CALL(pthread_setname_np, (thread->_name), "ConcurrentGCThread");
#elif defined(__linux__) || defined(hongmeng)
    CHECK_PTHREAD_CALL(prctl, (PR_SET_NAME, thread->_name), "ConcurrentGCThread");
#endif
    thread->run();
    return nullptr;
}

// gc/shared/concurrentGCThread.cpp:43-53. wait_init_completed() is a
// HotSpot init-order guard; this runtime starts GC threads after Heap::Init.
void ConcurrentGCThread::run()
{
    run_service();

    // Signal thread has terminated
    std::lock_guard<std::mutex> ml(_terminator_lock);
    _has_terminated.store(true, std::memory_order_release);
    _terminator_condition.notify_all();
}

// gc/shared/concurrentGCThread.cpp:55-69. The wait for termination is the
// join of the OS thread: HotSpot threads are detached and self-deleting, this
// runtime reclaims the pthread so the object can be destroyed afterwards.
void ConcurrentGCThread::stop()
{
    CHECK_DETAIL(!should_terminate(), "Invalid state");
    CHECK_DETAIL(!has_terminated(), "Invalid state");

    // Signal thread to terminate
    _should_terminate.store(true, std::memory_order_seq_cst);

    stop_service();

    // Wait for thread to terminate
    {
        std::unique_lock<std::mutex> ml(_terminator_lock);
        while (!_has_terminated.load(std::memory_order_relaxed)) {
            _terminator_condition.wait(ml);
        }
    }
    CHECK_PTHREAD_CALL(pthread_join, (_thread, nullptr), "ConcurrentGCThread");
}

bool ConcurrentGCThread::should_terminate() const
{
    return _should_terminate.load(std::memory_order_acquire);
}

bool ConcurrentGCThread::has_terminated() const
{
    return _has_terminated.load(std::memory_order_acquire);
}
} // namespace MapleRuntime
