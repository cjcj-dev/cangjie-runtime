// Synthetic heap/request fixture only; not a mark-start acceptance entry.
#ifndef MRT_GC_CYCLE_SEQUENCE_FIXTURE_HPP
#define MRT_GC_CYCLE_SEQUENCE_FIXTURE_HPP
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zRememberedSet.hpp"
namespace MapleRuntime {
struct GenerationSequenceFixture {
    static void Advance(ZGeneration& cycle);
    static void AdvanceYoung(ZGeneration& cycle)
    {
        Advance(cycle);
        ZRememberedSet::flip();
    }
};

}
#endif
