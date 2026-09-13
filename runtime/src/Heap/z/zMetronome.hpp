#pragma once
#include <cstdint>
namespace MapleRuntime {
class GcMetronome {
public:
    explicit GcMetronome(uint64_t startNs, uint64_t intervalNs = 10000000)
        : startNs(startNs), intervalNs(intervalNs) {}

    uint64_t DeadlineNs() const { return startNs + intervalNs * ticks; }

    bool Poll(uint64_t nowNs)
    {
        const uint64_t deadline = DeadlineNs();
        if (nowNs < deadline) {
            return false;
        }
        const uint64_t overslept = nowNs - deadline;
        if (overslept > intervalNs) {
            ticks += overslept / intervalNs;
        }
        ++ticks;
        return true;
    }

private:
    const uint64_t startNs;
    const uint64_t intervalNs;
    uint64_t ticks = 1;
};
}
