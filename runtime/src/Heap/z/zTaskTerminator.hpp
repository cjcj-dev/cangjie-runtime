// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#ifndef MRT_Z_TASK_TERMINATOR_HPP
#define MRT_Z_TASK_TERMINATOR_HPP
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "Heap/z/zTaskQueue.hpp"

namespace MapleRuntime {
// HotSpot gc/shared/taskTerminator.cpp:39-218. std::mutex/condition_variable
// provide the Monitor lock, unlock and timed-wait operations.
class TaskTerminator {
    class DelayContext {
        unsigned yieldCount = 0;
        unsigned hardSpinCount;
        unsigned hardSpinLimit;
        void reset_hard_spin_information()
        {
            hardSpinCount = 0;
            hardSpinLimit = 4096 >> 10;
        }
    public:
        DelayContext() { reset_hard_spin_information(); }
        bool needs_sleep() const { return yieldCount >= 5000; }
        void do_step()
        {
            ++yieldCount;
            if (hardSpinCount > 10) {
                std::this_thread::yield();
                reset_hard_spin_information();
            } else {
                for (unsigned i = 0; i < hardSpinLimit; ++i) {
#if defined(__x86_64__)
                    __builtin_ia32_pause();
#elif defined(__aarch64__)
                    __asm__ volatile("yield" ::: "memory");
#else
                    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
                }
                ++hardSpinCount;
                hardSpinLimit = std::min(2 * hardSpinLimit, 4096U);
            }
        }
    };
    const unsigned nthreads;
    TaskQueueSetSuper* const queueSet;
    unsigned offeredTermination = 0;
    std::mutex blocker;
    std::condition_variable condition;
    std::thread::id spinMaster;
    size_t tasks_in_queue_set() const { return queueSet->tasks(); }
    bool exit_termination(size_t tasks) const { return tasks > 0; }
    void prepare_for_return(std::thread::id self, size_t tasks = SIZE_MAX)
    {
        if (spinMaster == self) spinMaster = {};
        if (tasks >= offeredTermination - 1) condition.notify_all();
        else for (; tasks > 1; --tasks) condition.notify_one();
    }
public:
    TaskTerminator(unsigned nthreads, TaskQueueSetSuper* queueSet) : nthreads(nthreads), queueSet(queueSet) {}
    bool offer_termination()
    {
        if (nthreads == 1) {
            offeredTermination = 1;
            return true;
        }
        const auto self = std::this_thread::get_id();
        std::unique_lock<std::mutex> lock(blocker);
        ++offeredTermination;
        if (offeredTermination == nthreads) {
            prepare_for_return(self);
            return true;
        }
        for (;;) {
            if (spinMaster == std::thread::id{}) {
                spinMaster = self;
                DelayContext delay;
                while (!delay.needs_sleep()) {
                    lock.unlock();
                    delay.do_step();
                    const size_t tasks = tasks_in_queue_set();
                    const bool shouldExit = exit_termination(tasks);
                    lock.lock();
                    if (offeredTermination == nthreads) {
                        prepare_for_return(self);
                        return true;
                    } else if (shouldExit) {
                        prepare_for_return(self, tasks);
                        --offeredTermination;
                        return false;
                    }
                }
                spinMaster = {};
            }
            const bool timedOut = condition.wait_for(lock, std::chrono::milliseconds(1)) == std::cv_status::timeout;
            if (offeredTermination == nthreads) {
                prepare_for_return(self);
                return true;
            } else if (!timedOut) {
                prepare_for_return(self, 0);
                --offeredTermination;
                return false;
            } else {
                const size_t tasks = tasks_in_queue_set();
                if (exit_termination(tasks)) {
                    prepare_for_return(self, tasks);
                    --offeredTermination;
                    return false;
                }
            }
        }
    }
};
} // namespace MapleRuntime
#endif
