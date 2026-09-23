// Synthetic heap setup only. No product implementation is compiled here.
#define MRT_GENERATION_SEQUENCE_FIXTURE 1
#include "gc_cycle_sequence_fixture.hpp"
#undef MRT_GENERATION_SEQUENCE_FIXTURE

namespace MapleRuntime {
void GenerationSequenceFixture::Advance(ZGeneration& cycle)
{
    CHECK(cycle._seqnum != UINT32_MAX);
    ++cycle._seqnum;
}
}
