#ifndef SHARE_GC_Z_ZDIRECTOR_HPP
#define SHARE_GC_Z_ZDIRECTOR_HPP

#include "Heap/z/zThread.hpp"
#include <condition_variable>
#include <mutex>
#include "Base/Macros.h"

namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
struct ZDirectorSampleForTest {
    uint64_t serial = 0;
    uint64_t oldSequence = 0;
    size_t oldUsed = 0;
    size_t oldLive = 0;
    size_t softMaxCapacity = 0;
    size_t relocationHeadroom = 0;
    bool majorBusy = false;
};
#endif
class ZDirector : public ZThread {
private:
    static const uint64_t DecisionHz = 100;
    static ZDirector* _director;
    std::mutex monitor;
    std::condition_variable condition;
    bool stopped = false;
    bool reevaluate = false;
    bool minorBusy = false;
    bool majorBusy = false;

    bool wait_for_tick();

public:
    void run_thread() override;
    void terminate() override;

public:
    ZDirector();

    static void evaluate_rules();
#if defined(MRT_TESTABLE_INTERNALS)
    MRT_EXPORT static ZDirectorSampleForTest ReadSampleForTest();
#endif
    void notify_reevaluate();
    void set_busy(bool minor, bool busy);
    bool busy(bool minor);
};
} // namespace MapleRuntime

#endif
