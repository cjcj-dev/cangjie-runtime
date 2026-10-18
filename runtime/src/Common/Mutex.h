// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#ifndef MRT_COMMON_MUTEX_H
#define MRT_COMMON_MUTEX_H

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace MapleRuntime {
struct ThreadLocalData;
class InFlightMutexRelease;

// HotSpot mutex.hpp/mutex.cpp: the platform lock and its logical owner are
// separate, so a transition can release a lock before acknowledging a pause.
class Mutex {
public:
    Mutex() = default;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    void lock();
    void unlock();
    bool owned_by_self() const;
    bool is_locked() const { return owner.load(std::memory_order_acquire) != nullptr; }
protected:
    void set_owner(ThreadLocalData* value) { owner.store(value, std::memory_order_release); }
    std::mutex platformLock;
private:
    friend class InFlightMutexRelease;
    void lock_contended();
    std::atomic<ThreadLocalData*> owner{nullptr};
};

class Monitor : public Mutex {
public:
    void wait();
    void notify_all();
private:
    std::condition_variable condition;
};

class MutexLocker {
public:
    explicit MutexLocker(Mutex& mutex) : mutex(mutex) { mutex.lock(); }
    ~MutexLocker() { mutex.unlock(); }
    MutexLocker(const MutexLocker&) = delete;
    MutexLocker& operator=(const MutexLocker&) = delete;
private:
    Mutex& mutex;
};

class MonitorLocker : public MutexLocker {
public:
    explicit MonitorLocker(Monitor& monitor) : MutexLocker(monitor), monitor(monitor) {}
    void wait() { monitor.wait(); }
    void notify_all() { monitor.notify_all(); }
private:
    Monitor& monitor;
};

// One process-lifetime Heap_lock, shared by GC caller windows, services,
// pending references and the final VM-thread rendezvous.
Monitor& HeapMonitor();
} // namespace MapleRuntime
#endif
