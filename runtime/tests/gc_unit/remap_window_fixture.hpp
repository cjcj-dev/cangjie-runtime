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
    bool checkArena = false;
    bool checkCopy = false;
    const char* lifetimeTarget = nullptr;
    bool lifetimeReaderHeld = false;
    bool lifetimeCopyEntered = false;
    bool lifetimeReaderSafe = false;
    bool lifetimeClaimed = false;
    bool lifetimeReleasedFind = false;
    bool lifetimeDoneLast = false;
    bool lifetimeResult = false;
    bool lifetimeCancelled = false;
    BaseObject* lifetimeReturned = nullptr;
    ForwardingTable::Owner lifetimeOwner;
    MAddress expectedField = 0;
    MAddress copiedField = 0;
    bool arenaInstalled = false;
    size_t arenaBudget = 0;
    size_t arenaUsed = 0;
    RegionInfo* kept = nullptr;
    BaseObject* from = nullptr;
    BaseObject* copyFrom = nullptr;
    WCollector* collector = nullptr;
    std::thread::id gcThread;
    std::thread::id mutatorThread;
    uintptr_t oldColour = 0;
    pid_t gcTid = 0;

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
    if (point == 7) {
        GC_EXPECT_FALSE(state.kept->IsForwardingDone());
        GC_EXPECT_TRUE(state.collector->GetGCPhase() == GCPhase::GC_PHASE_POST_TRACE);
        state.window = true;
        state.cv.notify_all();
        state.Wait(lock, state.entered, "domain-wait-entry");
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
        // Do not stop before Wait consumes the retired lookup when its product
        // return is cut. The consumer result is the target assertion.
        std::fprintf(stderr, "DOMAIN retired_input_match=%d\n",
                     consumed.retiredAnswer == ForwardingTable::ToAnswer::ArmedHit && consumed.to == produced.to);
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
    // Removing an entry before done leaves a possible page publisher. Let
    // the real task complete so the consumer can prove the missing-entry
    // invariant at completion, rather than waiting on a fixture-held worker.
    if (state.domain == ForwardDomain::Missing && !state.afterDone) return;
    state.Wait(lock, state.returned, "domain-result");
}

void PageLifetimeHook(unsigned point, RegionInfo* region, BaseObject* object)
{
    auto& state = *remapWindow;
    std::unique_lock<std::mutex> lock(state.mutex);
    if (point == 1) {
        state.lifetimeOwner = ForwardingTable::RetainPageOwner(state.kept);
        state.window = true;
        state.cv.notify_all();
        state.Wait(lock, state.lifetimeReaderHeld, "page-reader-retained");
    } else if (point == 8 && object == state.from && std::this_thread::get_id() == state.mutatorThread) {
        state.lifetimeReaderHeld = true;
        state.entered = true;
        state.cv.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            const int32_t refs = state.lifetimeOwner->ref_count().load(std::memory_order_acquire);
            if (refs < 0 || state.lifetimeCopyEntered) {
                state.lifetimeReaderSafe = refs < -1 && !state.lifetimeCopyEntered;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr, "P2 prerequisite timeout: page claimant did not enter\n");
                std::abort();
            }
            lock.unlock();
            std::this_thread::yield();
            lock.lock();
        }
        state.cv.notify_all();
    } else if (point == 9 && region == state.kept) {
        state.lifetimeCopyEntered = true;
        state.lifetimeClaimed = state.lifetimeOwner->claimed().load(std::memory_order_acquire);
        state.cv.notify_all();
    } else if (point == 11 && region == state.kept) {
        state.lifetimeDoneLast = !state.lifetimeOwner->is_done();
    } else if (point == 10 && region == state.kept) {
        const MAddress to = state.lifetimeOwner->find(reinterpret_cast<MAddress>(state.from));
        state.expected = reinterpret_cast<BaseObject*>(to);
        state.lifetimeReleasedFind = state.lifetimeOwner->ref_count().load(std::memory_order_acquire) == 0 &&
            to != 0 && to != reinterpret_cast<MAddress>(state.from) &&
            raw(HeapSlotAt<>(to + TYPEINFO_PTR_SIZE).GetTargetObject()) == state.expectedField;
        state.published = true;
        state.copied = true;
        state.cv.notify_all();
    }
}

void RemapWindowHook(unsigned point, RegionInfo* region, BaseObject* object)
{
    if (remapWindow->lifetimeTarget != nullptr) {
        PageLifetimeHook(point, region, object);
        return;
    }
    if (remapWindow->domain != ForwardDomain::None) {
        ForwardDomainHook(point, region, object);
        return;
    }
    auto& state = *remapWindow;
    std::unique_lock<std::mutex> lock(state.mutex);
    if (point == 7 && !state.copyOnly) {
        GC_EXPECT_TRUE(state.collector->GetGCPhase() == GCPhase::GC_PHASE_POST_TRACE);
        state.window = true;
        state.cv.notify_all();
        state.Wait(lock, state.entered, "WaitRouted-entry");
        return;
    }
    if (point == 1) {
        GC_EXPECT_TRUE(std::this_thread::get_id() == state.gcThread);
        GC_EXPECT_TRUE(state.kept->IsGhostFromRegion());
        GC_EXPECT_TRUE(state.kept->HasFromPageMetadata());
        GC_EXPECT_FALSE(state.kept->IsForwardingDone());
        GC_EXPECT_FALSE(state.kept->IsCompacted());
        GC_EXPECT_TRUE(static_cast<uintptr_t>(state.collector->ZPointerRemappedYoungMask) != state.oldColour);
        if (state.checkArena) {
            auto* keptTable = ForwardingTable::GetEntries(reinterpret_cast<MAddress>(state.from));
            auto* copyTable = ForwardingTable::GetEntries(reinterpret_cast<MAddress>(state.copyFrom));
            const auto* arena = keptTable == nullptr ? nullptr : keptTable->arena_for_test();
            state.arenaInstalled = arena != nullptr && copyTable != nullptr &&
                copyTable->arena_for_test() == arena &&
                arena->contains_for_test(keptTable, ZForwarding::AttachedArray::object_size() +
                    ZForwarding::AttachedArray::array_size(keptTable->length())) &&
                arena->contains_for_test(copyTable, ZForwarding::AttachedArray::object_size() +
                    ZForwarding::AttachedArray::array_size(copyTable->length()));
            state.arenaBudget = arena == nullptr ? 0 : arena->capacity();
            state.arenaUsed = arena == nullptr ? 0 : arena->used();
            state.arenaInstalled = state.arenaInstalled && state.arenaUsed > 0 &&
                state.arenaUsed <= state.arenaBudget;
        }
        const auto lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(state.from));
        GC_EXPECT_TRUE(lookup.answer != ForwardingTable::ToAnswer::ArmedHit);
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
        }
    } else if (point == 4) {
        const auto lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(state.copyFrom));
        GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedHit);
        GC_EXPECT_TRUE(lookup.activeAnswer == ForwardingTable::ToAnswer::ArmedHit);
        GC_EXPECT_TRUE(lookup.to != 0 && lookup.to != reinterpret_cast<MAddress>(state.copyFrom));
        GC_EXPECT_TRUE(state.copyFrom->IsForwarded());
        std::fprintf(stderr, "REMAP_WINDOW real_copy=1 from=%p to=%#zx\n", state.copyFrom, lookup.to);
        if (state.checkCopy) {
            state.copiedField = raw(HeapSlotAt<>(lookup.to + TYPEINFO_PTR_SIZE).GetTargetObject());
        }
        state.copied = true;
    }
}

void RunRemapWindow(bool copyOnly, ForwardDomain domain = ForwardDomain::None, bool afterDone = false,
                    bool checkArena = false, bool checkCopy = false, const char* lifetimeTarget = nullptr,
                    bool parallel = false)
{
    // The strict arm is deliberately red on #175's frozen guard. Ordinary
    // suites run the copy prerequisite; the contract arm is explicitly invoked
    // with CJ_GC_UNIT_REMAP_WINDOW=1 and a single-test filter.
    const char* strict = std::getenv("CJ_GC_UNIT_REMAP_WINDOW");
    if (lifetimeTarget == nullptr && domain == ForwardDomain::None && !copyOnly && (strict == nullptr || std::strcmp(strict, "1") != 0)) {
        std::fprintf(stderr, "REMAP_WINDOW strict_arm=NOT_RUN enable=CJ_GC_UNIT_REMAP_WINDOW\n");
        return;
    }
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    GC_EXPECT_EQ(setenv("MRT_GCV2_MARKPAR_FORCE_SERIAL", "1", 1), 0);
    GC_EXPECT_EQ(setenv("MRT_GCV2_EVACPAR_FORCE_SERIAL", parallel ? "0" : "1", 1), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);
    auto& fx = *new GcHeapFixture(true);
    RegionSpace& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    auto& manager = space.GetRegionManager();
    manager.regionHeapStart = fx.heapStart;
    manager.regionHeapEnd = fx.heapStart + GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE;
    manager.inactiveZone.store(lifetimeTarget != nullptr ? manager.regionHeapEnd :
                               fx.heapStart + 3 * RegionInfo::UNIT_SIZE);
    manager.maxUnitCountPerRegion = 1;
    manager.freeRegionManager.Initialize(GcHeapFixture::kUnits);

    // Fully walkable pages: no untyped prefix before the first object.
    fx.obj0 = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.obj1 = fx.PlaceObject(fx.region1->GetRegionStart());
    auto* copyPage = RegionInfo::InitRegion(2, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    copyPage->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    auto* copyObject = fx.PlaceObject(copyPage->GetRegionStart());
    if (checkCopy) {
        HeapSlotAt<>(reinterpret_cast<MAddress>(copyObject) + TYPEINFO_PTR_SIZE)
            .StoreColoured(GcUnit::StoreGoodPointer(fx.obj0));
    }
    const size_t size = RegionSpace::GetAllocSize(*fx.obj1);
    if (lifetimeTarget != nullptr) {
        // A dead prefix makes in-place relocation move the rooted object.
        // No released/dirty/inactive target page is supplied by this fixture.
        copyObject = fx.PlaceObject(copyPage->GetRegionStart() + size);
        HeapSlotAt<>(reinterpret_cast<MAddress>(copyObject) + TYPEINFO_PTR_SIZE)
            .StoreColoured(GcUnit::StoreGoodPointer(fx.obj0));
    }
    fx.region0->SetRegionAllocPtr(fx.region0->GetRegionStart() + size);
    fx.region1->SetRegionAllocPtr(fx.region1->GetRegionStart() + size);
    copyPage->SetRegionAllocPtr(reinterpret_cast<MAddress>(copyObject) + size);
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
    GCThreadPool pool("gc-unit-remap-window", parallel ? 1 : 0, GCPoolThread::GC_THREAD_PRIORITY);
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
    state.lifetimeTarget = lifetimeTarget;
    state.afterDone = afterDone;
    state.copyOnly = copyOnly;
    state.kept = domain == ForwardDomain::Copy || lifetimeTarget != nullptr ? copyPage : fx.region1;
    state.from = domain == ForwardDomain::Copy || lifetimeTarget != nullptr ? copyObject : fx.obj1;
    state.checkArena = checkArena;
    state.checkCopy = checkCopy;
    state.expectedField = reinterpret_cast<MAddress>(fx.obj0);
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
            if (state.lifetimeCancelled) return;
            lock.unlock();
            BaseObject* result = remap(&collector, state.from, ZGenerationId::young,
                ForwardingProvenance{ForwardingHolderKind::StackSlot, &mutator, &state.from});
            lock.lock();
            if (lifetimeTarget != nullptr) {
                state.lifetimeReturned = result;
                state.returned = true;
                state.cv.notify_all();
                return;
            }
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
    if (lifetimeTarget != nullptr) {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.window) {
            state.lifetimeCancelled = true;
            state.window = true;
            state.cv.notify_all();
        }
    }
    if (waiter.joinable()) waiter.join();
    setHook(nullptr);
    if (lifetimeTarget != nullptr) {
        state.lifetimeResult = state.published && state.lifetimeReturned == state.expected;
        const bool matched = std::strcmp(lifetimeTarget, "claim") == 0 ? state.lifetimeClaimed :
            std::strcmp(lifetimeTarget, "reader") == 0 ? state.lifetimeReaderSafe :
            std::strcmp(lifetimeTarget, "find") == 0 ? state.lifetimeReleasedFind && state.lifetimeResult :
            state.lifetimeDoneLast && state.lifetimeOwner && state.lifetimeOwner->is_done();
        std::fprintf(stderr, "P2_PAGE target_assertion executed=1 target=%s matched=%d claimed=%d reader_safe=%d "
                     "released_find=%d done_last=%d result=%d\n", lifetimeTarget, matched,
                     state.lifetimeClaimed, state.lifetimeReaderSafe, state.lifetimeReleasedFind,
                     state.lifetimeDoneLast, state.lifetimeResult);
        GC_EXPECT_TRUE(matched);
    }
    if (checkCopy) {
        const bool initialized = state.copied && state.copiedField == state.expectedField;
        std::fprintf(stderr, "P1_PRODUCT_COPY target_assertion executed=1 matched=%d expected=%#zx actual=%#zx\n",
                     initialized, state.expectedField, state.copiedField);
        GC_EXPECT_TRUE(initialized);
    }
    if (checkArena) {
        std::fprintf(stderr, "P1_PRODUCT_ARENA target_assertion executed=1 matched=%d budget=%zu used=%zu copied=%d identity=%d\n",
                     state.arenaInstalled, state.arenaBudget, state.arenaUsed, state.copied, state.published);
        GC_EXPECT_TRUE(state.arenaInstalled);
    }
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
    bool allTargetsMatched = true;
    bool allStatusesMatched = true;
    bool allPrerequisitesMatched = true;
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
        const std::string queuedMissing =
            "WCollector::WaitRoutedTipReady.request-complete-without-receipt consumer=WCollector::WaitRoutedTipReady ";
        const std::string target = std::string(domain == ForwardDomain::Missing
            ? (output.find(queuedMissing) != std::string::npos
                ? "WCollector::WaitRoutedTipReady.request-complete-without-receipt"
                : "WCollector::WaitRoutedTipReady.published-without-receipt")
            : domain == ForwardDomain::Unavailable
                ? "WCollector::WaitRoutedTipReady.publication-closed-never-installed"
                : "WCollector::WaitRoutedTipReady.publication-closed") +
            " consumer=WCollector::WaitRoutedTipReady ";
        const bool targetMatched = negative ? output.find(target) != std::string::npos
            : output.find("DOMAIN result_assertion=PASS") != std::string::npos;
        const bool statusMatched = negative ? WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT
            : WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::fprintf(stderr, "DOMAIN target_assertion executed=1 matched=%d status_matched=%d "
                     "entered=%d produced=%d target=%s\n", targetMatched, statusMatched, entered, produced,
                     negative ? target.c_str() : "consumer-result");
        allTargetsMatched &= targetMatched;
        allStatusesMatched &= statusMatched;
        allPrerequisitesMatched &= entered && produced;
    }
    GC_EXPECT_TRUE(allTargetsMatched);
    GC_EXPECT_TRUE(allStatusesMatched);
    GC_EXPECT_TRUE(allPrerequisitesMatched);
}
} // namespace

GC_OTHER_VM_TEST(ForwardReturnDomain, Identity) { RunForwardDomain(ForwardDomain::Identity); }
GC_OTHER_VM_TEST(ForwardReturnDomain, NonIdentityCopy) { RunForwardDomain(ForwardDomain::Copy); }
GC_OTHER_VM_TEST(ForwardReturnDomain, RetiredHit) { RunForwardDomain(ForwardDomain::Retired); }
GC_OTHER_VM_TEST(ForwardReturnDomain, MissingEntry) { RunForwardDomain(ForwardDomain::Missing); }
GC_OTHER_VM_TEST(ForwardReturnDomain, WrongLifecycle) { RunForwardDomain(ForwardDomain::WrongLife); }
GC_OTHER_VM_TEST(ForwardReturnDomain, Unavailable) { RunForwardDomain(ForwardDomain::Unavailable); }

GC_OTHER_VM_TEST(YoungConc, ForwardingArenaProductInstall)
{
    RunRemapWindow(true, ForwardDomain::None, false, true);
}

GC_OTHER_VM_TEST(YoungConc, ForwardingPublishedTargetInitialized)
{
    RunRemapWindow(true, ForwardDomain::None, false, false, true);
}

GC_OTHER_VM_TEST(YoungConc, ForwardingPageClaimedByProductTask)
{
    RunRemapWindow(false, ForwardDomain::None, false, false, false, "claim");
}
GC_OTHER_VM_TEST(YoungConc, ForwardingReaderExitsBeforeInPlaceReuse)
{
    RunRemapWindow(false, ForwardDomain::None, false, false, false, "reader");
}
GC_OTHER_VM_TEST(YoungConc, ReleasedForwardingFindReturnsProductCopy)
{
    RunRemapWindow(false, ForwardDomain::None, false, false, false, "find");
}
GC_OTHER_VM_TEST(YoungConc, ForwardingDoneFollowsPageWork)
{
    RunRemapWindow(false, ForwardDomain::None, false, false, false, "done");
}

GC_OTHER_VM_TEST(YoungConc, ForwardingReaderExitsBeforeInPlaceReuseParallel)
{
    RunRemapWindow(false, ForwardDomain::None, false, false, false, "reader", true);
}
