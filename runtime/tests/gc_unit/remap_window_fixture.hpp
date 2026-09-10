// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Included by test_young_conc.cpp to share its runtime-dispatch fixture.
// zRelocate.cpp:382-415: retain refused -> wait -> consume forwarding receipt.
#include <chrono>
#include <dlfcn.h>

namespace {
struct RemapWindow {
    std::mutex mutex;
    std::condition_variable cv;
    bool window = false;
    bool entered = false;
    bool published = false;
    bool returned = false;
    bool copied = false;
    bool copyOnly = false;
    RegionInfo* kept = nullptr;
    BaseObject* from = nullptr;
    BaseObject* copyFrom = nullptr;
    WCollector* collector = nullptr;
    std::thread::id gcThread;
    std::thread::id mutatorThread;
    uintptr_t oldColour = 0;
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

void RemapWindowHook(unsigned point, RegionInfo* region, BaseObject* object)
{
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
        GC_EXPECT_FALSE(region->IsForwardingDone());
        GC_EXPECT_FALSE(region->IsCompacted());
        std::fprintf(stderr,
                     "REMAP_WINDOW kept_identity=1 active_armed_hit=1 done=0 compacted=0 "
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

void RunRemapWindow(bool copyOnly)
{
    // The strict arm is deliberately red on #175's frozen guard. Ordinary
    // suites run the copy prerequisite; the contract arm is explicitly invoked
    // with CJ_GC_UNIT_REMAP_WINDOW=1 and a single-test filter.
    if (!copyOnly && std::getenv("CJ_GC_UNIT_REMAP_WINDOW") == nullptr) {
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
    state.copyOnly = copyOnly;
    state.kept = fx.region1;
    state.from = fx.obj1;
    state.copyFrom = copyObject;
    state.collector = &collector;
    state.gcThread = std::this_thread::get_id();
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
            GC_EXPECT_TRUE(result == state.from);
            GC_EXPECT_FALSE(state.kept->IsForwardingDone());
            std::fprintf(stderr, "REMAP_WINDOW target_guard_returned_identity=1 done=0\n");
            state.returned = true;
            state.cv.notify_all();
            ThreadLocal::SetMutator(nullptr);
        });
    }
    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
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
