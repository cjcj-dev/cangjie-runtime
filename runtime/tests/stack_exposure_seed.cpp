#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include "Mutator/Mutator.h"
#include "UnwindStack/StackExposureHook.h"

using MapleRuntime::Mutator;
using MapleRuntime::StackExposureHook;
using MapleRuntime::StackWatermark;

extern "C" void RevSeedExposure()
{
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr) {
        std::fprintf(stderr, "GATE_FAIL reason=null_mutator_at_seed\n");
        std::_Exit(92);
    }
    StackWatermark& watermark = mutator->GetStackWatermark();
    constexpr size_t frames = 64;
    watermark.Reset();
    const bool began = watermark.TryBegin(0x52565755, StackWatermark::WM_OWNER_SELF, frames);
    if (!began) {
        std::fprintf(stderr, "GATE_FAIL reason=watermark_begin\n");
        std::_Exit(93);
    }
    StackExposureHook::ResetStats();
    std::fprintf(stderr, "GATE_SEED phase=%u owner=%u cursor=%zu frames=%zu\n",
                 static_cast<unsigned>(watermark.GetPhase()),
                 static_cast<unsigned>(watermark.GetOwner()), watermark.GetCursorIndex(),
                 watermark.GetFrameCount());
}

extern "C" uint64_t RevReportExposure()
{
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr) {
        std::fprintf(stderr, "GATE_FAIL reason=null_mutator_at_report\n");
        return 0;
    }
    StackWatermark& watermark = mutator->GetStackWatermark();
    const size_t fire = StackExposureHook::FireCount();
    const size_t advance = StackExposureHook::AdvanceCount();
    const size_t processOne = StackExposureHook::ProcessOneCount();
    const size_t cross = StackExposureHook::CrossWithoutProcessCount();
    const size_t stw = StackExposureHook::StopTheWorldCallsInHook();
    std::fprintf(stderr,
                 "GATE_SUMMARY fire=%zu advance=%zu processOne=%zu cross=%zu stw=%zu cursor=%zu phase=%u owner=%u\n",
                 fire, advance, processOne, cross, stw, watermark.GetCursorIndex(),
                 static_cast<unsigned>(watermark.GetPhase()),
                 static_cast<unsigned>(watermark.GetOwner()));
    return (static_cast<uint64_t>(fire) << 32) | static_cast<uint64_t>(processOne);
}
