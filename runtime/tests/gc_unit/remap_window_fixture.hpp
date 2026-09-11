// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Included by test_young_conc.cpp to share its runtime-dispatch fixture.
// zRelocate.cpp:382-415: retain refused -> wait -> consume forwarding receipt.
#include <chrono>
#include <dlfcn.h>

namespace {
enum class ForwardDomain { None, Identity, Copy, Retired, Missing, WrongLife, Unavailable };

struct RemapWindow {
    std::mutex mutex;
    std::condition_variable cv;
    bool window = false;
    bool entered = false;
    bool published = false;
    bool returned = false;
    bool copied = false;
    bool copyOnly = false;
    ForwardDomain domain = ForwardDomain::None;
    bool afterDone = false;
    BaseObject* expected = nullptr;
    RegionInfo* kept = nullptr;
    BaseObject* from = nullptr;
    BaseObject* copyFrom = nullptr;
    WCollector* collector = nullptr;
    std::thread::id gcThread;
    std::thread::id mutatorThread;
    uintptr_t oldColour = 0;
    pid_t gcTid = 0;
    std::unique_ptr<RegionInfo::DrainScope> drain;

    void Wait(std::unique_lock<std::mutex>& lock, const bool& flag, const char* stage)
    {
        if (!cv.wait_for(lock, std::chrono::seconds(10), [&] { return flag; })) {
            std::fprintf(stderr, "REMAP_WINDOW_TIMEOUT stage=%s\n", stage);
            std::fflush(stderr);
            std::abort();
        }
    }
};
RemapWindow* remapWindow = nullptr;

void ForwardDomainHook(unsigned point, RegionInfo* region, BaseObject* object)
{
    auto& state = *remapWindow;
    std::unique_lock<std::mutex> lock(state.mutex);
    if (point == 1) {
        GC_EXPECT_FALSE(state.kept->IsForwardingDone());
        state.drain.reset(new RegionInfo::DrainScope(state.kept, MutatorRelocate::Retire::DISPEL_GHOST));
        state.window = true;
        state.cv.notify_all();
        state.Wait(lock, state.entered, "domain-wait-entry");
        // The copying worker needs its page token; the mutator has already
        // passed retain and is parked inside the real WaitRoutedTipReady.
        if (state.domain == ForwardDomain::Copy) state.drain.reset();
        return;
    }
    if (point == 2 && object == state.from) {
        GC_EXPECT_TRUE(std::this_thread::get_id() == state.mutatorThread);
        GC_EXPECT_TRUE(ThreadLocal::GetMutator() != nullptr);
        GC_EXPECT_TRUE(region == state.kept);
        GC_EXPECT_FALSE(region->IsForwardingDone());
        std::fprintf(stderr, "DOMAIN wait_entry=1 from=%p tls=%p\n", object,
                     static_cast<void*>(ThreadLocal::GetMutator()));
        state.entered = true;
        state.cv.notify_all();
        state.Wait(lock, state.published, "domain-publication");
        return;
    }
    const bool copied = state.domain == ForwardDomain::Copy;
    const bool selected = copied ? (state.afterDone ? point == 4 : point == 6 && object == state.from)
        : region == state.kept && point == (state.afterDone ? 5u : 3u);
    if (!selected) return;

    const MAddress from = reinterpret_cast<MAddress>(state.from);
    const auto produced = ForwardingTable::LookupTo(from);
    GC_EXPECT_TRUE(produced.answer == ForwardingTable::ToAnswer::ArmedHit);
    GC_EXPECT_TRUE(produced.activeAnswer == ForwardingTable::ToAnswer::ArmedHit);
    GC_EXPECT_EQ(produced.to == from, !copied);
    GC_EXPECT_EQ(state.kept->IsForwardingDone(), state.afterDone);
    if (copied) GC_EXPECT_TRUE(state.from->IsForwarded());
    state.expected = reinterpret_cast<BaseObject*>(produced.to);
    std::fprintf(stderr, "DOMAIN producer_receipt=1 copy=%d done=%d from=%p to=%p\n",
                 copied, state.afterDone, state.from, state.expected);

    if (state.domain == ForwardDomain::Retired) {
        ForwardingTable::ClearEntries(state.kept->GetRegionStart(), state.kept->GetRegionSize());
    } else if (state.domain == ForwardDomain::Missing || state.domain == ForwardDomain::Unavailable) {
        // Remove exactly the receipt that the real producer just installed.
        // This is an invalid-input fixture, never an alternate publisher.
        auto* table = ForwardingTable::GetEntries(from);
        GC_EXPECT_TRUE(table != nullptr);
        ForwardingCursor cursor = 0;
        GC_EXPECT_TRUE(table->find(table->index(from), &cursor).populated());
        table->entries()[cursor].store(0, std::memory_order_release);
        if (state.domain == ForwardDomain::Unavailable) {
            ForwardingTable::ForcePublicationClosedForTest(from);
        }
    } else if (state.domain == ForwardDomain::WrongLife) {
        const auto route = state.kept->GetRouteState();
        state.kept->BumpRegionLifeId();
        // Keep the diagnostic route carrier current; the table and receipt
        // retain their original, now mismatching lifecycle stamps.
        state.kept->SetRouteState(route);
    }
    const auto consumed = ForwardingTable::LookupTo(from);
    std::fprintf(stderr, "DOMAIN input answer=%u active=%u retired=%u cause=%u to=%#zx done=%d\n",
                 static_cast<unsigned>(consumed.answer), static_cast<unsigned>(consumed.activeAnswer),
                 static_cast<unsigned>(consumed.retiredAnswer), static_cast<unsigned>(consumed.unavailableCause),
                 consumed.to, state.kept->IsForwardingDone());
    if (state.domain == ForwardDomain::Retired) {
        GC_EXPECT_TRUE(consumed.retiredAnswer == ForwardingTable::ToAnswer::ArmedHit);
        GC_EXPECT_EQ(consumed.to, produced.to);
    } else if (state.domain == ForwardDomain::Missing) {
        GC_EXPECT_TRUE(consumed.answer == ForwardingTable::ToAnswer::ArmedMiss);
        GC_EXPECT_EQ(consumed.to, 0u);
    } else if (state.domain == ForwardDomain::WrongLife && !RegionLifeClock::EnforceEnabled()) {
        GC_EXPECT_TRUE(consumed.answer == ForwardingTable::ToAnswer::ArmedHit);
        GC_EXPECT_EQ(consumed.to, produced.to);
        std::fprintf(stderr, "DOMAIN lifecycle_audit_control=ArmedHit\n");
    } else if (state.domain == ForwardDomain::WrongLife || state.domain == ForwardDomain::Unavailable) {
        GC_EXPECT_TRUE(consumed.answer == ForwardingTable::ToAnswer::Unavailable);
        GC_EXPECT_EQ(consumed.to, 0u);
    }
    state.published = true;
    state.cv.notify_all();
    state.Wait(lock, state.returned, "domain-result");
}

void RemapWindowHook(unsigned point, RegionInfo* region, BaseObject* object)
{
    if (remapWindow->domain != ForwardDomain::None) {
        ForwardDomainHook(point, region, object);
        return;
    }
    auto& state = *remapWindow;
    std::unique_lock<std::mutex> lock(state.mutex);
    if (point == 1) {
        GC_EXPECT_TRUE(std::this_thread::get_id() == state.gcThread);
        GC_EXPECT_TRUE(state.kept->IsGhostFromRegion());
        GC_EXPECT_TRUE(state.kept->HasFromPageMetadata());
        GC_EXPECT_FALSE(state.kept->IsForwardingDone());
        GC_EXPECT_FALSE(state.kept->IsCompacted());
        GC_EXPECT_TRUE(static_cast<uintptr_t>(state.collector->ZPointerRemappedYoungMask) != state.oldColour);
        const auto lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(state.from));
        GC_EXPECT_TRUE(lookup.answer != ForwardingTable::ToAnswer::ArmedHit);
        if (!state.copyOnly) {
            // Hold the existing product retire token while the mutator attempts
            // retain. No table entry or done bit is fabricated by this fixture.
            state.drain.reset(new RegionInfo::DrainScope(state.kept, MutatorRelocate::Retire::DISPEL_GHOST));
            state.window = true;
            state.cv.notify_all();
            state.Wait(lock, state.entered, "WaitRouted-entry");
        }
        std::fprintf(stderr, "REMAP_WINDOW prepared=1 flipped=1 before_forward=1 wait_entered=%d\n",
                     state.entered);
    } else if (point == 2 && object == state.from) {
        GC_EXPECT_TRUE(std::this_thread::get_id() == state.mutatorThread);
        GC_EXPECT_TRUE(std::this_thread::get_id() != state.gcThread);
        GC_EXPECT_TRUE(ThreadLocal::GetMutator() != nullptr);
        std::fprintf(stderr, "REMAP_WINDOW wait_entry=1 mutator_tid=%d gc_tid=%d tls_mutator=%p\n",
                     static_cast<int>(MapleRuntime::GetTid()), static_cast<int>(state.gcTid),
                     static_cast<void*>(ThreadLocal::GetMutator()));
        GC_EXPECT_TRUE(region == state.kept);
        GC_EXPECT_FALSE(region->IsForwardingDone());
        state.entered = true;
        state.cv.notify_all();
        state.Wait(lock, state.published, "kept-publication");
    } else if (point == 3 && region == state.kept) {
        const auto lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(state.from));
        GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedHit);
        GC_EXPECT_TRUE(lookup.activeAnswer == ForwardingTable::ToAnswer::ArmedHit);
        GC_EXPECT_EQ(lookup.to, reinterpret_cast<MAddress>(state.from));
        GC_EXPECT_TRUE(region->IsGhostFromRegion());
        GC_EXPECT_TRUE(region->IsYoungRegion());
        GC_EXPECT_EQ(region->GetYoungAge(), 1u);
        GC_EXPECT_FALSE(region->IsForwardingDone());
        GC_EXPECT_FALSE(region->IsCompacted());
        std::fprintf(stderr,
                     "REMAP_WINDOW producer=FinishStayYoungInPlace kept_identity=1 active_armed_hit=1 done=0 compacted=0 "
                     "wait_entered=%d from=%p\n", state.entered, state.from);
        std::fflush(stderr);
        state.published = true;
        state.cv.notify_all();
        if (!state.copyOnly) {
            state.Wait(lock, state.returned, "post-WaitRouted-guard");
            state.drain.reset();
        }
    } else if (point == 4) {
        const auto lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(state.copyFrom));
        GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedHit);
        GC_EXPECT_TRUE(lookup.activeAnswer == ForwardingTable::ToAnswer::ArmedHit);
        GC_EXPECT_TRUE(lookup.to != 0 && lookup.to != reinterpret_cast<MAddress>(state.copyFrom));
        GC_EXPECT_TRUE(state.copyFrom->IsForwarded());
        std::fprintf(stderr, "REMAP_WINDOW real_copy=1 from=%p to=%#zx\n", state.copyFrom, lookup.to);
        state.copied = true;
    }
}

void RunRemapWindow(bool copyOnly, ForwardDomain domain = ForwardDomain::None, bool afterDone = false)
{
    // The strict arm is deliberately red on #175's frozen guard. Ordinary
    // suites run the copy prerequisite; the contract arm is explicitly invoked
    // with CJ_GC_UNIT_REMAP_WINDOW=1 and a single-test filter.
    const char* strict = std::getenv("CJ_GC_UNIT_REMAP_WINDOW");
    if (domain == ForwardDomain::None && !copyOnly && (strict == nullptr || std::strcmp(strict, "1") != 0)) {
        std::fprintf(stderr, "REMAP_WINDOW strict_arm=NOT_RUN enable=CJ_GC_UNIT_REMAP_WINDOW\n");
        return;
    }
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    GC_EXPECT_EQ(setenv("MRT_GCV2_MARKPAR_FORCE_SERIAL", "1", 1), 0);
    GC_EXPECT_EQ(setenv("MRT_GCV2_EVACPAR_FORCE_SERIAL", "1", 1), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);
    auto& fx = *new GcHeapFixture(true);
    RegionSpace& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    auto& manager = space.GetRegionManager();
    manager.regionHeapStart = fx.heapStart;
    manager.regionHeapEnd = fx.heapStart + GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE;
    manager.inactiveZone.store(fx.heapStart + 3 * RegionInfo::UNIT_SIZE);
    manager.maxUnitCountPerRegion = 1;
    manager.freeRegionManager.Initialize(GcHeapFixture::kUnits);

    // Fully walkable pages: no untyped prefix before the first object.
    fx.obj0 = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.obj1 = fx.PlaceObject(fx.region1->GetRegionStart());
    auto* copyPage = RegionInfo::InitRegion(2, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    copyPage->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    auto* copyObject = fx.PlaceObject(copyPage->GetRegionStart());
    const size_t size = RegionSpace::GetAllocSize(*fx.obj1);
    fx.region0->SetRegionAllocPtr(fx.region0->GetRegionStart() + size);
    fx.region1->SetRegionAllocPtr(fx.region1->GetRegionStart() + size);
    copyPage->SetRegionAllocPtr(copyPage->GetRegionStart() + size);
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(0);
    copyPage->SetYoungRegionFlag(1);
    copyPage->SetYoungAge(14);
    fx.PlantLiveInfo(fx.region1);
    fx.PlantLiveInfo(copyPage);
    manager.EnlistFullThreadLocalRegion(fx.region1);
    manager.EnlistFullThreadLocalRegion(copyPage);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    GCThreadPool pool("gc-unit-remap-window", 0, GCPoolThread::GC_THREAD_PRIORITY);
    RelocationReceiptTestAccess::BindThreadPool(resources, &pool);
    Heap::GetHeap().GetRememberedSet().Initialize(fx.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    RememberedSet producerRememberedSet;
    producerRememberedSet.Initialize(fx.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    TestTraceBarrier barrier(collector, producerRememberedSet);
    Mutator producer;
    ThreadLocal::SetMutator(&producer);
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_YOUNG;
    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    barrier.Record(fx.obj0, reinterpret_cast<MAddress>(field), fx.obj1);
    barrier.Record(fx.obj0, reinterpret_cast<MAddress>(field), copyObject);
    producer.FlushSatbBuffer();
    ThreadLocal::SetMutator(nullptr);

    void* so = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
    GC_EXPECT_TRUE(so != nullptr);
    using SetHook = void (*)(void (*)(unsigned, RegionInfo*, BaseObject*));
    auto setHook = reinterpret_cast<SetHook>(dlsym(so, "MRT_SetRemapWindowTestHook"));
    using Remap = BaseObject* (*)(const WCollector*, BaseObject*, ZGenerationId, const ForwardingProvenance&);
    auto remap = reinterpret_cast<Remap>(dlsym(so,
        "_ZNK12MapleRuntime10WCollector24relocate_or_remap_objectEPNS_10BaseObjectENS_13ZGenerationIdERKNS_20ForwardingProvenanceE"));
    GC_EXPECT_TRUE(setHook != nullptr && remap != nullptr);
    Dl_info identity{};
    GC_EXPECT_TRUE(dladdr(reinterpret_cast<void*>(remap), &identity) != 0);
    std::fprintf(stderr, "REMAP_WINDOW product=%s symbol=%s\n", identity.dli_fname, identity.dli_sname);

    RemapWindow state;
    state.domain = domain;
    state.afterDone = afterDone;
    state.copyOnly = copyOnly;
    state.kept = domain == ForwardDomain::Copy ? copyPage : fx.region1;
    state.from = domain == ForwardDomain::Copy ? copyObject : fx.obj1;
    state.copyFrom = copyObject;
    state.collector = &collector;
    state.gcThread = std::this_thread::get_id();
    state.gcTid = MapleRuntime::GetTid();
    state.oldColour = static_cast<uintptr_t>(collector.ZPointerRemappedYoungMask);
    remapWindow = &state;
    setHook(RemapWindowHook);
    std::thread waiter;
    if (!copyOnly) {
        waiter = std::thread([&] {
            Mutator mutator;
            ThreadLocal::SetMutator(&mutator);
            std::unique_lock<std::mutex> lock(state.mutex);
            state.mutatorThread = std::this_thread::get_id();
            state.Wait(lock, state.window, "flip-window");
            lock.unlock();
            BaseObject* result = remap(&collector, state.from, ZGenerationId::young,
                ForwardingProvenance{ForwardingHolderKind::StackSlot, &mutator, &state.from});
            lock.lock();
            GC_EXPECT_TRUE(state.entered && state.published);
            if (domain != ForwardDomain::None) {
                // Exit this isolated scenario at the observed consumer. Fault
                // fixtures must not resume GC with intentionally invalid input.
                std::fprintf(stderr, "DOMAIN consumer_return=%p expected=%p done=%d\n", result,
                             state.expected, state.kept->IsForwardingDone());
                if (domain == ForwardDomain::Identity || domain == ForwardDomain::Copy ||
                    domain == ForwardDomain::Retired ||
                    (domain == ForwardDomain::WrongLife && !RegionLifeClock::EnforceEnabled())) {
                    GC_EXPECT_TRUE(result == state.expected);
                    GC_EXPECT_EQ(state.kept->IsForwardingDone(), afterDone);
                    std::fprintf(stderr, "DOMAIN result_assertion=PASS\n");
                }
                std::fflush(nullptr);
                _exit(0);
            }
            GC_EXPECT_TRUE(result == state.from);
            GC_EXPECT_FALSE(state.kept->IsForwardingDone());
            std::fprintf(stderr, "REMAP_WINDOW target_guard_returned_identity=1 done=0\n");
            state.returned = true;
            state.cv.notify_all();
            ThreadLocal::SetMutator(nullptr);
        });
    }
    try {
        RelocationReceiptTestAccess::RunCollectionDispatch(collector);
    } catch (const std::exception& error) {
        // Preserve the exact failing invariant before an outstanding waiter
        // prevents normal C++ teardown in this isolated process.
        std::fprintf(stderr, "REMAP_WINDOW target_assertion=%s\n", error.what());
        std::fflush(stderr);
        std::abort();
    }
    if (waiter.joinable()) waiter.join();
    setHook(nullptr);
    GC_EXPECT_TRUE(state.copied && state.published);
    if (!copyOnly) GC_EXPECT_TRUE(state.returned && state.entered);
    // OTHER_VM owns the mapped heap and product metadata until exit.
    // No teardown may re-interpret a page whose forwarding life was retired.
    RelocationReceiptTestAccess::BindThreadPool(resources, nullptr);
    pool.Exit();
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    std::fflush(stderr);
}
} // namespace

GC_OTHER_VM_TEST(YoungConc, RemapWindowRealCopy)
{
    RunRemapWindow(true);
}
GC_OTHER_VM_TEST(YoungConc, KeptIdentityAtWaitRoutedGuard)
{
    RunRemapWindow(false);
}

namespace {
void RunForwardDomain(ForwardDomain domain)
{
    const char* enabled = std::getenv("CJ_GC_UNIT_FORWARD_DOMAIN");
    if (enabled == nullptr || std::strcmp(enabled, "1") != 0) {
        std::fprintf(stderr, "DOMAIN NOT_RUN enable=CJ_GC_UNIT_FORWARD_DOMAIN\n");
        return;
    }
    const bool negative = domain == ForwardDomain::Missing ||
                          (domain == ForwardDomain::WrongLife && RegionLifeClock::EnforceEnabled()) ||
                          domain == ForwardDomain::Unavailable;
    for (bool done : {false, true}) {
        int pipefd[2];
        GC_EXPECT_EQ(pipe(pipefd), 0);
        std::fflush(nullptr);
        const pid_t child = fork();
        GC_EXPECT_TRUE(child >= 0);
        if (child == 0) {
            close(pipefd[0]);
            if (dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
            close(pipefd[1]);
            (void)signal(SIGABRT, SIG_DFL);
            RunRemapWindow(false, domain, done);
            _exit(3); // A successful scenario exits from its real consumer.
        }
        close(pipefd[1]);
        std::string output;
        char buffer[4096];
        ssize_t count;
        while ((count = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
            output.append(buffer, static_cast<size_t>(count));
        }
        close(pipefd[0]);
        int status = 0;
        GC_EXPECT_EQ(waitpid(child, &status, 0), child);
        std::fprintf(stderr, "DOMAIN scenario=%u done=%d status=%d\n%s",
                     static_cast<unsigned>(domain), done, status, output.c_str());
        // Keep prerequisites separate from the named target assertion. Print
        // all decisions even when a prerequisite failed, so an earlier failure
        // cannot masquerade as execution of the consumer assertion.
        const bool entered = output.find("DOMAIN wait_entry=1") != std::string::npos;
        const bool produced = output.find("DOMAIN producer_receipt=1") != std::string::npos;
        const char* target = domain == ForwardDomain::Missing
            ? (done ? "WCollector::WaitRoutedTipReady.published-without-receipt"
                    : "WCollector::WaitRoutedTipReady.retain-refused-without-receipt")
            : "WCollector::WaitRoutedTipReady.publication-closed";
        const bool targetMatched = negative ? output.find(target) != std::string::npos
            : output.find("DOMAIN result_assertion=PASS") != std::string::npos;
        const bool statusMatched = negative ? WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT
            : WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::fprintf(stderr, "DOMAIN target_assertion executed=1 matched=%d status_matched=%d "
                     "entered=%d produced=%d target=%s\n", targetMatched, statusMatched, entered, produced,
                     negative ? target : "consumer-result");
        GC_EXPECT_TRUE(targetMatched);
        GC_EXPECT_TRUE(statusMatched);
        GC_EXPECT_TRUE(entered && produced);
    }
}
} // namespace

GC_OTHER_VM_TEST(ForwardReturnDomain, Identity) { RunForwardDomain(ForwardDomain::Identity); }
GC_OTHER_VM_TEST(ForwardReturnDomain, NonIdentityCopy) { RunForwardDomain(ForwardDomain::Copy); }
GC_OTHER_VM_TEST(ForwardReturnDomain, RetiredHit) { RunForwardDomain(ForwardDomain::Retired); }
GC_OTHER_VM_TEST(ForwardReturnDomain, MissingEntry) { RunForwardDomain(ForwardDomain::Missing); }
GC_OTHER_VM_TEST(ForwardReturnDomain, WrongLifecycle) { RunForwardDomain(ForwardDomain::WrongLife); }
GC_OTHER_VM_TEST(ForwardReturnDomain, Unavailable) { RunForwardDomain(ForwardDomain::Unavailable); }
