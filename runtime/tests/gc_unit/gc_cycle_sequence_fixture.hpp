// Synthetic heap/request fixture only; not a mark-start acceptance entry.
#ifndef MRT_GC_CYCLE_SEQUENCE_FIXTURE_HPP
#define MRT_GC_CYCLE_SEQUENCE_FIXTURE_HPP
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zRememberedSet.hpp"
namespace MapleRuntime {
struct GenerationSequenceFixture {
    static void Advance(GenerationCycle& cycle);
    static void AdvanceYoung(GenerationCycle& cycle)
    {
        Advance(cycle);
        ZRememberedSet::flip();
    }
};

}
#endif
