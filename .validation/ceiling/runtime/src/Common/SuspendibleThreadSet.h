#ifndef MRT_SUSPENDIBLE_THREAD_SET_H
#define MRT_SUSPENDIBLE_THREAD_SET_H

#include <atomic>
#include <cstdint>
#include "Heap/z/zBarrier.hpp"

namespace MapleRuntime {
class SuspendibleThreadSet : public AllStatic {
    friend class SuspendibleThreadSetJoiner;
    friend class SuspendibleThreadSetLeaver;
private:
    static uint32_t nthreads;
    static uint32_t nthreadsStopped;
    static std::atomic<bool> suspendAll;
    static double suspendAllStart;
    static bool is_synchronized();
    static void join();
    static void leave();
    static void yield_slow();
public:
    static bool should_yield() { return suspendAll.load(std::memory_order_relaxed); }
    static void yield()
    {
        if (should_yield()) {
            yield_slow();
        }
    }
    static void synchronize();
    static void desynchronize();
};

class SuspendibleThreadSetJoiner {
private:
    bool active;
public:
    explicit SuspendibleThreadSetJoiner(bool active = true) : active(active)
    {
        if (this->active) {
            SuspendibleThreadSet::join();
        }
    }
    ~SuspendibleThreadSetJoiner()
    {
        if (active) {
            SuspendibleThreadSet::leave();
        }
    }
    bool should_yield()
    {
        return active && SuspendibleThreadSet::should_yield();
    }
    void yield() { SuspendibleThreadSet::yield(); }
};

class SuspendibleThreadSetLeaver {
private:
    bool active;
public:
    explicit SuspendibleThreadSetLeaver(bool active = true) : active(active)
    {
        if (this->active) {
            SuspendibleThreadSet::leave();
        }
    }
    ~SuspendibleThreadSetLeaver()
    {
        if (active) {
            SuspendibleThreadSet::join();
        }
    }
};

class ZRendezvousGCThreads {
public:
    void doit()
    {
        SuspendibleThreadSet::synchronize();
        SuspendibleThreadSet::desynchronize();
    }
};
} // namespace MapleRuntime
#endif
