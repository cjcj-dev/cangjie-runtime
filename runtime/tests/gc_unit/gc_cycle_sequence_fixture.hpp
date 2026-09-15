// Synthetic heap/request fixture only; not a mark-start acceptance entry.
#ifndef MRT_GC_CYCLE_SEQUENCE_FIXTURE_HPP
#define MRT_GC_CYCLE_SEQUENCE_FIXTURE_HPP
#define MRT_GENERATION_SEQUENCE_FIXTURE 1
#include "Heap/z/zGeneration.hpp"
#undef MRT_GENERATION_SEQUENCE_FIXTURE
namespace MapleRuntime {
struct GenerationSequenceFixture {
    static void Advance(GenerationCycle& cycle)
    {
        std::lock_guard<std::mutex> lock(cycle.mutex);
        CHECK(cycle.active);
        CHECK(cycle.sequence != UINT64_MAX);
        ++cycle.sequence;
    }
    template<class Remembered>
    static void AdvanceYoung(GenerationCycle& cycle, Remembered& remembered)
    {
        Advance(cycle);
        remembered.FlipForMinor();
    }
};
}
#endif
