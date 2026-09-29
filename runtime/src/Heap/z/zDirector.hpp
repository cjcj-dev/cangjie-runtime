#ifndef SHARE_GC_Z_ZDIRECTOR_HPP
#define SHARE_GC_Z_ZDIRECTOR_HPP

#include "Heap/z/zThread.hpp"
#include "Heap/z/zLock.inline.hpp"
#include "Base/Macros.h"

namespace MapleRuntime {
class ZDirector : public ZThread {
private:
    static const uint64_t DecisionHz = 100;
    static ZDirector* _director;
    ZConditionLock monitor;
    bool stopped = false;

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
