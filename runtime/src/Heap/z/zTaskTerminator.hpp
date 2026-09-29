// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#ifndef MRT_Z_TASK_TERMINATOR_HPP
#define MRT_Z_TASK_TERMINATOR_HPP
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include "Heap/z/zTaskQueue.hpp"
#include "Heap/z/zLock.inline.hpp"

namespace MapleRuntime {
// HotSpot gc/shared/taskTerminator.cpp:39-218. The existing condition lock
// supplies Monitor lock, unlock, notification and timed-wait operations.
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
    unsigned nthreads;
    TaskQueueSetSuper* const queueSet;
    alignas(64) unsigned offeredTermination = 0;
    alignas(64) ZConditionLock blocker;
    std::thread::id spinMaster;
    size_t tasks_in_queue_set() const { return queueSet->tasks(); }
    void assert_queue_set_empty() const { queueSet->assert_empty(); }
    bool exit_termination(size_t tasks, TerminatorTerminator* terminator) const
    {
        return tasks > 0 || (terminator != nullptr && terminator->should_exit_termination());
    }
    void prepare_for_return(std::thread::id self, size_t tasks = SIZE_MAX)
    {
        if (spinMaster == self) spinMaster = {};
        if (tasks >= offeredTermination - 1) blocker.notify_all();
        else for (; tasks > 1; --tasks) blocker.notify();
    }
public:
    TaskTerminator(unsigned nthreads, TaskQueueSetSuper* queueSet) : nthreads(nthreads), queueSet(queueSet) {}
    ~TaskTerminator()
    {
        assert(offeredTermination == 0 || offeredTermination == nthreads);
        assert(spinMaster == std::thread::id{});
    }
    void reset_for_reuse()
    {
        if (offeredTermination != 0) {
            assert(offeredTermination == nthreads);
            assert(spinMaster == std::thread::id{});
            offeredTermination = 0;
        }
    }
    void reset_for_reuse(unsigned threads)
    {
        reset_for_reuse();
        nthreads = threads;
    }
    bool offer_termination() { return offer_termination(nullptr); }
    bool offer_termination(TerminatorTerminator* terminator)
    {
        assert(nthreads > 0);
        if (nthreads == 1) {
            offeredTermination = 1;
            assert_queue_set_empty();
            return true;
        }
        const auto self = std::this_thread::get_id();
        std::unique_lock<ZConditionLock> lock(blocker);
        assert(offeredTermination < nthreads);
        ++offeredTermination;
        if (offeredTermination == nthreads) {
            prepare_for_return(self);
            assert_queue_set_empty();
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
                    const bool shouldExit = exit_termination(tasks, terminator);
                    lock.lock();
                    if (offeredTermination == nthreads) {
                        prepare_for_return(self);
                        assert_queue_set_empty();
                        return true;
                    } else if (shouldExit) {
                        prepare_for_return(self, tasks);
                        --offeredTermination;
                        return false;
                    }
                }
                spinMaster = {};
            }
            const bool timedOut = blocker.wait(1);
            if (offeredTermination == nthreads) {
                prepare_for_return(self);
                assert_queue_set_empty();
                return true;
            } else if (!timedOut) {
                prepare_for_return(self, 0);
                --offeredTermination;
                return false;
            } else {
                const size_t tasks = tasks_in_queue_set();
                if (exit_termination(tasks, terminator)) {
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
