#ifndef SHARE_GC_Z_ZDIRECTOR_HPP
#define SHARE_GC_Z_ZDIRECTOR_HPP

#include "Heap/z/zThread.hpp"
#include <condition_variable>
#include <mutex>
#include "Base/Macros.h"

namespace MapleRuntime {
class ZDirector : public ZThread {
private:
    static const uint64_t DecisionHz = 100;
    static ZDirector* _director;
    std::mutex monitor;
    std::condition_variable condition;
    bool stopped = false;
    bool reevaluate = false;

    bool wait_for_tick();

public:
    void run_thread() override;
    void terminate() override;

public:
    ZDirector();

    static void evaluate_rules();
    void notify_reevaluate();
};
} // namespace MapleRuntime

#endif
