#include "gc_generation_test.hpp"
#include "gc_worker_fixture.hpp"
#include "CangjieRuntime.h"
#include "Cangjie.h"
#include "Heap/z/zAbort.inline.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zJNICritical.hpp"
#include "Heap/z/zMark.hpp"
#include "b09_runtime_fixture.hpp"
#include "gc_unittest.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include "Heap/z/zPage.hpp"
#include "Heap/z/zRemembered.hpp"
#include "gc_unittest.hpp"

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(ZGeneration, StaticYoungOldAndId)
{
    Heap::GetHeap();
    GC_EXPECT_TRUE(ZGeneration::young() != nullptr);
    GC_EXPECT_TRUE(ZGeneration::old() != nullptr);
    GC_EXPECT_TRUE(ZGeneration::young()->is_young());
    GC_EXPECT_TRUE(ZGeneration::old()->is_old());
    GC_EXPECT_EQ(static_cast<int>(ZGeneration::young()->id()), static_cast<int>(ZGenerationId::young));
    GC_EXPECT_EQ(static_cast<int>(ZGeneration::old()->id()), static_cast<int>(ZGenerationId::old));
    GC_EXPECT_TRUE(ZGeneration::generation(ZGenerationId::young) == static_cast<ZGeneration*>(ZGeneration::young()));
    GC_EXPECT_TRUE(ZGeneration::generation(ZGenerationId::old) == static_cast<ZGeneration*>(ZGeneration::old()));
}

GC_TEST(ZGeneration, ThreeStatePhase)
{
    Heap::GetHeap();
    ZGenerationYoung* young = ZGeneration::young();
    GC_EXPECT_TRUE(young != nullptr);
    young->set_phase(ZGeneration::Phase::Mark);
    GC_EXPECT_TRUE(young->is_phase_mark());
    GC_EXPECT_TRUE(!young->is_phase_mark_complete());
    GC_EXPECT_TRUE(!young->is_phase_relocate());
    young->set_phase(ZGeneration::Phase::MarkComplete);
    GC_EXPECT_TRUE(young->is_phase_mark_complete());
    young->set_phase(ZGeneration::Phase::Relocate);
    GC_EXPECT_TRUE(young->is_phase_relocate());
}

GC_OTHER_VM_TEST(ZAbort, AllStaticAbortpoint)
{
    GC_EXPECT_TRUE(!ZAbort::should_abort());
    ZAbort::abort();
    GC_EXPECT_TRUE(ZAbort::should_abort());
    ZAbort::abort();
    GC_EXPECT_TRUE(ZAbort::should_abort());
}

GC_OTHER_VM_TEST(ZCollectedHeap, StopAborts)
{
    Heap::GetHeap();
    ZCollectedHeap::stop();
    GC_EXPECT_TRUE(ZAbort::should_abort());
}

GC_RUNTIME_OTHER_VM_TEST(ZGeneration, CollectionScopeClearsTimer)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    ZGenerationYoung* young = ZGeneration::young();
    young->at_collection_start(young);
    GC_EXPECT_TRUE(young->gc_timer() == young);
    young->at_collection_end();
    GC_EXPECT_TRUE(young->gc_timer() == nullptr);
}

GC_TEST(ZGeneration, FreedPromotedCompactedAtomics)
{
    Heap::GetHeap();
    ZGenerationYoung* young = ZGeneration::young();
    young->reset_statistics();
    GC_EXPECT_EQ(young->freed(), static_cast<size_t>(0));
    young->increase_freed(16);
    young->increase_promoted(8);
    young->increase_compacted(4);
    GC_EXPECT_EQ(young->freed(), static_cast<size_t>(16));
    GC_EXPECT_EQ(young->promoted(), static_cast<size_t>(8));
    GC_EXPECT_EQ(young->compacted(), static_cast<size_t>(4));
    young->reset_statistics();
}

GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, InitializationPublishesConditionLock)
{
    RuntimeParam param{};
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    const bool drained = ZJNICritical::block_bounded(1000);
    const int64_t blocked = ZJNICritical::count_snapshot();
    ZJNICritical::unblock_bounded();
    const int64_t unblocked = ZJNICritical::count_snapshot();
    std::printf("ZJNI_INITIALIZATION_RESULT blocked=%lld unblocked=%lld\n",
                static_cast<long long>(blocked), static_cast<long long>(unblocked));
    GC_EXPECT_TRUE(drained);
    GC_EXPECT_EQ(blocked, static_cast<int64_t>(-1));
    GC_EXPECT_EQ(unblocked, static_cast<int64_t>(0));
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, BlockWaitsWhileEntered)
{
    RuntimeParam param{};
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    bool finishedWhileEntered = true;
    CJThreadHandle task = RunCJTask([](void* context) -> void* {
        ZJNICritical::enter();
        std::atomic<bool> finished{ false };
        std::atomic<bool> drained{ false };
        std::thread waiter([&] {
            const bool ok = ZJNICritical::block_bounded(10000);
            finished.store(true, std::memory_order_release);
            if (ok) {
                drained.store(true, std::memory_order_release);
                ZJNICritical::unblock_bounded();
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        *static_cast<bool*>(context) = finished.load(std::memory_order_acquire);
        std::printf("ZJNI_CRITICAL_BLOCK_WAIT finished=%d count=%lld\n",
                    *static_cast<bool*>(context),
                    static_cast<long long>(ZJNICritical::count_snapshot()));
        ZJNICritical::exit();
        waiter.join();
        // A bounded block that never drained after exit also fails the check below.
        if (!drained.load(std::memory_order_acquire)) {
            *static_cast<bool*>(context) = true;
        }
        return nullptr;
    }, &finishedWhileEntered);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &result), E_OK);
    ReleaseHandle(task);
    GC_EXPECT_FALSE(finishedWhileEntered);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// ZGC zGeneration.cpp:499-505 and zRemembered.cpp:347-355: the
// remembered set is ready when generation construction returns.
GC_TEST(RememberedLifecycle720, ConstructedGenerationPublishesHighestGranule)
{
    Heap::GetHeap();
    ZPageTable pages;
    RegionManager allocator;
    GenerationFixtureState::Scope generationState;
    ZGenerationOld old;
    ZGenerationYoung young(&pages, &old.forwarding_table(), &allocator);
    const size_t last = ZAddressOffsetMax - ZGranuleSize;
    ZPage page(ZPageType::large, PageAge::old,
               ZVirtualMemory(static_cast<zoffset>(last), ZGranuleSize));
    GC_EXPECT_EQ(pages.map().size(), ZAddressOffsetMax >> ZGranuleSizeShift);
    pages.insert(&page);
    ZRemsetTableIterator iter(young.remembered(), false);
    ZRemsetTableEntry entry{};
    const bool found = iter.next(&entry);
    std::fprintf(stderr, "REMEMBERED720 highest=%zu found=%d page_matches=%d\n",
                 last, found, entry._page == &page);
    GC_EXPECT_TRUE(found && entry._page == &page);
    GC_EXPECT_TRUE(!iter.next(&entry));
    pages.remove(&page);
}

GC_TEST(RememberedLifecycle720, HeapPublicationReachesConstructedRemembered)
{
    auto& heap = Heap::GetHeap();
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZPage* page = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, PageAge::old, flags);
    GC_EXPECT_TRUE(page != nullptr);
    ZRemsetTableIterator iter(heap.young().remembered(), false);
    ZRemsetTableEntry entry{};
    bool found = false;
    while (iter.next(&entry)) {
        found |= entry._page == page;
    }
    std::fprintf(stderr, "REMEMBERED720 heap_publication_found=%d\n", found);
    GC_EXPECT_TRUE(found);
    Heap::free_page(page);
}

GC_TEST(RememberedLifecycle720, UnboundConstructionAbortsRegisterFoundOld)
{
    Heap::GetHeap();
    const size_t last = ZAddressOffsetMax - ZGranuleSize;
    ZPage page(ZPageType::large, PageAge::old,
               ZVirtualMemory(static_cast<zoffset>(last), ZGranuleSize));
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        ZRemembered unbound(nullptr, &Heap::GetHeap().old().forwarding_table(),
                            &Heap::GetHeap().page_allocator());
        unbound.register_found_old(&page);
        std::_Exit(0);
    }
    int status = 0;
    GC_EXPECT_TRUE(waitpid(child, &status, 0) == child);
    std::fprintf(stderr, "REMEMBERED720 unbound_signaled=%d sig=%d\n",
                 WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    GC_EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

// ZGC zGeneration.cpp:1261-1294: mark_end publishes completion after end()
// succeeds; the collection owner consumes abort at its phase boundary.
GC_OTHER_VM_TEST(Lifecycle1310, OldMarkEndPublishesCompletionBeforeAbortpoint)
{
    B09RuntimeFixture runtime;
    auto& old = Heap::GetHeap().old();
    InitializeGenerationWorkers(old, 1);
    old.Mark().Start();
    old.Mark().PrepareWork();
    old.set_phase(ZGeneration::Phase::Mark);
    ZAbort::abort();
    const bool ended = old.mark_end();
    const bool complete = old.is_phase_mark_complete();
    std::fprintf(stderr, "MARKEND1310_TARGET ended=%d complete=%d abort=%d\n",
                 ended, complete, ZAbort::should_abort());
    GC_EXPECT_TRUE(ended && complete && ZAbort::should_abort());
    old.StopWorkers();
}

// Bounded JNI-critical block used by the collector (zJNICritical.cpp
// block_bounded/unblock_bounded): sharing, timeout restore, stuck fast path and
// the exit/timeout race.
namespace {
struct RdBoundedHolders {
    std::atomic<int> entered{ 0 };
    std::atomic<bool> release{ false };
    std::atomic<bool> acquirerStart{ false };
    std::atomic<bool> acquirerEntered{ false };
    std::atomic<bool> acquirerRelease{ false };
};

void* RdHoldCriticalUntilReleased(void* context)
{
    auto* state = static_cast<RdBoundedHolders*>(context);
    ZJNICritical::enter();
    state->entered.fetch_add(1, std::memory_order_acq_rel);
    while (!state->release.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ZJNICritical::exit();
    return nullptr;
}

void* RdEnterDuringBoundedBlock(void* context)
{
    auto* state = static_cast<RdBoundedHolders*>(context);
    while (!state->acquirerStart.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ZJNICritical::enter();
    state->acquirerEntered.store(true, std::memory_order_release);
    while (!state->acquirerRelease.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ZJNICritical::exit();
    return nullptr;
}

long long RdBoundedElapsedMs(std::chrono::steady_clock::time_point start)
{
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

template <typename Pred>
bool RdBoundedWaitFor(Pred pred, int seconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

struct RdRaceState {
    std::atomic<int> round{ -1 };
    std::atomic<int> go{ -1 };
    std::atomic<int> exited{ -1 };
    std::atomic<int> ack{ -1 };
    int rounds = 0;
};

void* RdRaceHolder(void* context)
{
    auto* state = static_cast<RdRaceState*>(context);
    uint32_t seed = 12345u;
    for (int i = 0; i < state->rounds; ++i) {
        ZJNICritical::enter();
        state->round.store(i, std::memory_order_release);
        while (state->go.load(std::memory_order_acquire) < i) {
            std::this_thread::yield();
        }
        seed = seed * 1103515245u + 12345u;
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds((seed >> 16) % 2000u);
        while (std::chrono::steady_clock::now() < until) {
        }
        ZJNICritical::exit();
        state->exited.store(i, std::memory_order_release);
        // Do not re-enter before the main thread has checked the drained count.
        while (state->ack.load(std::memory_order_acquire) < i) {
            std::this_thread::yield();
        }
    }
    return nullptr;
}
} // namespace

GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, BoundedBlockSharedAndReleasedByLastParty)
{
    RuntimeParam param{};
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    const bool first = ZJNICritical::block_bounded(1000);
    const bool second = ZJNICritical::block_bounded(1000);
    const int64_t shared = ZJNICritical::count_snapshot();
    ZJNICritical::unblock_bounded();
    const int64_t afterFirst = ZJNICritical::count_snapshot();
    ZJNICritical::unblock_bounded();
    const int64_t afterLast = ZJNICritical::count_snapshot();
    std::printf("ZJNI_BOUNDED_SHARED first=%d second=%d shared=%lld after_first=%lld after_last=%lld\n",
                first ? 1 : 0, second ? 1 : 0, static_cast<long long>(shared),
                static_cast<long long>(afterFirst), static_cast<long long>(afterLast));
    GC_EXPECT_TRUE(first);
    GC_EXPECT_TRUE(second);
    GC_EXPECT_EQ(shared, static_cast<int64_t>(-1));
    GC_EXPECT_EQ(afterFirst, static_cast<int64_t>(-1));
    GC_EXPECT_EQ(afterLast, static_cast<int64_t>(0));
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, BoundedBlockTimeoutRestoresHoldersAndReleasesAcquirer)
{
    RuntimeParam param{};
    param.coParam.processorNum = 4;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    RdBoundedHolders state;
    CJThreadHandle first = RunCJTask(RdHoldCriticalUntilReleased, &state);
    CJThreadHandle second = RunCJTask(RdHoldCriticalUntilReleased, &state);
    CJThreadHandle acquirer = RunCJTask(RdEnterDuringBoundedBlock, &state);
    GC_EXPECT_TRUE(first != nullptr && second != nullptr && acquirer != nullptr);
    const bool holdersIn = RdBoundedWaitFor([&] { return state.entered.load(std::memory_order_acquire) == 2; }, 5);
    const int64_t before = ZJNICritical::count_snapshot();
    std::atomic<bool> enteredWhileBlocked{ true };
    std::thread starter([&] {
        (void)RdBoundedWaitFor([] { return ZJNICritical::count_snapshot() < 0; }, 5);
        state.acquirerStart.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        enteredWhileBlocked.store(state.acquirerEntered.load(std::memory_order_acquire), std::memory_order_release);
    });
    const auto blockStart = std::chrono::steady_clock::now();
    const bool drained = ZJNICritical::block_bounded(200);
    const long long waited = RdBoundedElapsedMs(blockStart);
    starter.join();
    const bool acquirerIn = RdBoundedWaitFor([&] { return state.acquirerEntered.load(std::memory_order_acquire); }, 5);
    const int64_t restored = ZJNICritical::count_snapshot();
    // The holders are still inside: the next bounded block reports the timeout at
    // once and never makes the count negative (no other acquirer is held out).
    const auto againStart = std::chrono::steady_clock::now();
    const bool again = ZJNICritical::block_bounded(200);
    const long long againMs = RdBoundedElapsedMs(againStart);
    const int64_t afterAgain = ZJNICritical::count_snapshot();
    state.acquirerRelease.store(true, std::memory_order_release);
    state.release.store(true, std::memory_order_release);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(first, &taskResult), E_OK);
    GC_EXPECT_EQ(GetTaskRet(second, &taskResult), E_OK);
    GC_EXPECT_EQ(GetTaskRet(acquirer, &taskResult), E_OK);
    ReleaseHandle(first);
    ReleaseHandle(second);
    ReleaseHandle(acquirer);
    const int64_t drainedCount = ZJNICritical::count_snapshot();
    // Draining to 0 clears the stuck mark: an ordinary bounded block succeeds again.
    const bool cleared = ZJNICritical::block_bounded(1000);
    const int64_t blocked = ZJNICritical::count_snapshot();
    if (cleared) {
        ZJNICritical::unblock_bounded();
    }
    const int64_t finalCount = ZJNICritical::count_snapshot();
    std::printf("ZJNI_BOUNDED_TIMEOUT before=%lld drained=%d waited=%lld entered_while_blocked=%d restored=%lld "
                "again=%d again_ms=%lld after_again=%lld end=%lld cleared=%d blocked=%lld final=%lld\n",
                static_cast<long long>(before), drained ? 1 : 0, waited,
                enteredWhileBlocked.load(std::memory_order_acquire) ? 1 : 0, static_cast<long long>(restored),
                again ? 1 : 0, againMs, static_cast<long long>(afterAgain), static_cast<long long>(drainedCount),
                cleared ? 1 : 0, static_cast<long long>(blocked), static_cast<long long>(finalCount));
    GC_EXPECT_TRUE(holdersIn);
    GC_EXPECT_EQ(before, static_cast<int64_t>(2));
    GC_EXPECT_FALSE(drained);
    GC_EXPECT_TRUE(waited >= 200);
    GC_EXPECT_FALSE(enteredWhileBlocked.load(std::memory_order_acquire));
    GC_EXPECT_TRUE(acquirerIn);
    GC_EXPECT_EQ(restored, static_cast<int64_t>(3));
    GC_EXPECT_FALSE(again);
    GC_EXPECT_TRUE(againMs < 20);
    GC_EXPECT_EQ(afterAgain, static_cast<int64_t>(3));
    GC_EXPECT_EQ(drainedCount, static_cast<int64_t>(0));
    GC_EXPECT_TRUE(cleared);
    GC_EXPECT_EQ(blocked, static_cast<int64_t>(-1));
    GC_EXPECT_EQ(finalCount, static_cast<int64_t>(0));
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, BoundedBlockTimeoutRacingExitKeepsCount)
{
    RuntimeParam param{};
    param.coParam.processorNum = 4;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    RdRaceState race;
    race.rounds = 300;
    CJThreadHandle holder = RunCJTask(RdRaceHolder, &race);
    GC_EXPECT_TRUE(holder != nullptr);
    int drainedRounds = 0;
    int timedOutRounds = 0;
    int badCount = 0;
    int lost = 0;
    for (int i = 0; i < race.rounds; ++i) {
        if (!RdBoundedWaitFor([&] { return race.round.load(std::memory_order_acquire) == i; }, 5)) {
            ++lost;
            break;
        }
        race.go.store(i, std::memory_order_release);
        if (ZJNICritical::block_bounded(1)) {
            ++drainedRounds;
            ZJNICritical::unblock_bounded();
        } else {
            ++timedOutRounds;
        }
        if (!RdBoundedWaitFor([&] { return race.exited.load(std::memory_order_acquire) == i; }, 5)) {
            ++lost;
            break;
        }
        if (ZJNICritical::count_snapshot() != 0) {
            ++badCount;
        }
        race.ack.store(i, std::memory_order_release);
    }
    // Release the holder if a round was lost.
    race.go.store(race.rounds, std::memory_order_release);
    race.ack.store(race.rounds, std::memory_order_release);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(holder, &taskResult), E_OK);
    ReleaseHandle(holder);
    // No stale stuck mark: with a holder inside, a bounded block waits for its exit.
    RdBoundedHolders state;
    CJThreadHandle last = RunCJTask(RdHoldCriticalUntilReleased, &state);
    const bool lastIn = RdBoundedWaitFor([&] { return state.entered.load(std::memory_order_acquire) == 1; }, 5);
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        state.release.store(true, std::memory_order_release);
    });
    const bool waitedForExit = ZJNICritical::block_bounded(2000);
    const int64_t blocked = ZJNICritical::count_snapshot();
    if (waitedForExit) {
        ZJNICritical::unblock_bounded();
    }
    releaser.join();
    GC_EXPECT_EQ(GetTaskRet(last, &taskResult), E_OK);
    ReleaseHandle(last);
    const int64_t finalCount = ZJNICritical::count_snapshot();
    std::printf("ZJNI_BOUNDED_RACE rounds=%d drained=%d timed_out=%d bad_count=%d lost=%d waited_for_exit=%d "
                "blocked=%lld final=%lld\n",
                race.rounds, drainedRounds, timedOutRounds, badCount, lost, waitedForExit ? 1 : 0,
                static_cast<long long>(blocked), static_cast<long long>(finalCount));
    GC_EXPECT_EQ(lost, 0);
    GC_EXPECT_EQ(badCount, 0);
    GC_EXPECT_EQ(drainedRounds + timedOutRounds, race.rounds);
    GC_EXPECT_TRUE(lastIn);
    GC_EXPECT_TRUE(waitedForExit);
    GC_EXPECT_EQ(blocked, static_cast<int64_t>(-1));
    GC_EXPECT_EQ(finalCount, static_cast<int64_t>(0));
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
