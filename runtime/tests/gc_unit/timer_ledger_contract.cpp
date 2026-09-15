// Product-linked Timer contract harness: the implementation under test comes from runtime headers/SO.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <type_traits>

#include "Base/GcLog.h"
#include "Base/LogFile.h"

using MapleRuntime::GcLog;
using MapleRuntime::FINALIZE;
using MapleRuntime::Timer;

namespace {
void CloseCycle(uint64_t seq)
{
    constexpr uint64_t durationNs = 1000000000;
    GcLog::Cycle(seq, "minor", "timer_contract", 1, durationNs, 9, 8, 1, 8, 10);
}
} // namespace

int main()
{
    (void)setenv("MRT_GC_LOG", "1", 1);
    if (std::is_copy_constructible_v<Timer> || std::is_copy_assignable_v<Timer> ||
        std::is_move_constructible_v<Timer> || std::is_move_assignable_v<Timer>) {
        std::puts("TIMER_LEDGER_CONTRACT_FAIL type_traits");
        return 1;
    }

    std::unique_ptr<MapleRuntime::GCIdMark> nestedId(new MapleRuntime::GCIdMark());
    const uint64_t nestedSeq = GcLog::CurrentSeq();
    {
        Timer root("contract.root");
        {
            Timer middle("contract.middle");
            {
                Timer deep("contract.deep");
            }
        }
    }
    GcLog::Stw("timer_nested", 1, 1, 2);
    CloseCycle(nestedSeq);
    nestedId.reset();

    std::unique_ptr<MapleRuntime::GCIdMark> capturedId(new MapleRuntime::GCIdMark());
    const uint64_t capturedSeq = GcLog::CurrentSeq();
    std::unique_ptr<Timer> captured(new Timer("cycle.captured"));
    GcLog::Stw("timer_captured", 1, 1, 2);
    CloseCycle(capturedSeq);
    capturedId.reset();
    captured.reset();

    std::unique_ptr<MapleRuntime::GCIdMark> ownershipId(new MapleRuntime::GCIdMark());
    const uint64_t ownershipSeq = GcLog::CurrentSeq();
    {
        Timer external("contract.external", FINALIZE);
    }
    {
        Timer internalNonpillar("young.flush_alloc");
    }
    GcLog::Stw("timer_ownership", 1, 1, 2);
    CloseCycle(ownershipSeq);
    ownershipId.reset();

    {
        Timer finalizer("Finalizer", FINALIZE);
    }
    std::puts("TIMER_LEDGER_CONTRACT_OK");
    return 0;
}
