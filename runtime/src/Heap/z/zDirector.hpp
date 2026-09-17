#ifndef SHARE_GC_Z_ZDIRECTOR_HPP
#define SHARE_GC_Z_ZDIRECTOR_HPP

#include "Heap/z/zThread.hpp"

namespace MapleRuntime {
class CollectorResources;

class ZDirector : public ZThread {
private:
    static const uint64_t DecisionHz = 100;
    static ZDirector* _director;
    CollectorResources& resources;

    bool wait_for_tick();

public:
    void run_thread() override;
    void terminate() override;

public:
    explicit ZDirector(CollectorResources& resources);

    static void evaluate_rules();
};
} // namespace MapleRuntime

#endif
