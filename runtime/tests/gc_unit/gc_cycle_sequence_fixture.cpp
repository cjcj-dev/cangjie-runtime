// Synthetic heap setup only. No product implementation is compiled here.
#define MRT_GENERATION_SEQUENCE_FIXTURE 1
#include "gc_cycle_sequence_fixture.hpp"
#undef MRT_GENERATION_SEQUENCE_FIXTURE

namespace MapleRuntime {
void GenerationSequenceFixture::Advance(GenerationCycle& cycle)
{
    std::lock_guard<std::mutex> lock(cycle.mutex);
    CHECK(cycle.active);
    CHECK(cycle.sequence != UINT64_MAX);
    ++cycle.sequence;
}
}
