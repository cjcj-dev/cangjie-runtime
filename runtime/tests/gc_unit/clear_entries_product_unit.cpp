// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_cycle_sequence_fixture.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <vector>

#include "gc_heap_fixture.hpp"
#include "zunittest.hpp"
#include "Heap/z/zCrossVM.hpp"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zObjectAllocator.hpp"
#include "Heap/z/zUtils.inline.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zUncoloredRoot.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
#include "Loader/ElfUnloadQuiescence.h"
#include "ObjectModel/RefField.inline.h"
#include "ObjectModel/MArray.inline.h"
#include "TypeInfoManager.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" int CJ_ScheduleManagerInit();

namespace MapleRuntime {

class RelocationReceiptTest {
public:
    template<Generation G>
    static void PrepareProductPage(ZPage*)
    {
    }

    static void ParkFrom(RegionManager&, ZPage* region)
    {
        region->SetRegionRole(ZPageRole::From);
    }

    static void ReleaseListOwnership(ZPage*)
    {
    }

    static void BindCollector(Heap* collector)
    {
        if (collector == nullptr) return;
        CHECK(collector == &Heap::GetHeap());
        for (ZGenerationId generation : {ZGenerationId::young, ZGenerationId::old}) {
            auto& cycle = Heap::GetHeap().GetZGeneration(generation);
            if (cycle.Sequence() != 0) continue;
            if (!cycle.Snapshot().active) cycle.Begin(0);
            if (generation == ZGenerationId::young) {
                alignas(8) uint64_t storage[16] {};
                RememberedSet empty;
                empty.Initialize(reinterpret_cast<MAddress>(storage), sizeof(storage));
                GenerationSequenceFixture::AdvanceYoung(cycle);
            } else {
                GenerationSequenceFixture::Advance(cycle);
            }
        }
    }

    static void Exempt(RegionManager& manager, ZPage* region)
    {
        manager.ExemptFromRegion(region);
    }

    static RefField<> QualifyStoreValue(Heap& collector, BaseObject* value)
    {
        return ZBarrier::GetAndTryTagRefField(value);
    }

    static BaseObject* ResolveStoreValue(Heap& collector, BaseObject* value)
    {
        const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, value, &value };
        return ZRelocate::ResolveStoreValue(value, provenance, Generation::Old);
    }

    static void CheckStoreGoodTarget(Heap& collector, BaseObject* value)
    {
        ZBarrier::CheckStoreGoodTarget("ForwardingLookupWitness", value,
            ForwardingProvenance{ ForwardingHolderKind::HeapRef, value, &value });
    }

    static BaseObject* ForwardUpdateRawRef(Heap& collector, ObjectRef& root)
    {
        const zaddress_unsafe observed = root.LoadPlain();
        BaseObject* oldObj = to_object(safe(observed));
        if (oldObj == nullptr || !Heap::IsHeapAddress(oldObj)) {
            return oldObj;
        }
        BaseObject* mapped = Heap::GetHeap().old().relocate_or_remap_object(oldObj);
        ZUncoloredRoot::process_no_keepalive(reinterpret_cast<zaddress_unsafe*>(&root), ZPointerLoadGoodMask);
        return mapped;
    }

    static bool FixMinorField(Heap& collector, RefField<>& field, BaseObject* knownBase = nullptr)
    {
        return ZRelocate::FixMinorEvacuatedSlot(field, knownBase, nullptr);
    }

    static BaseObject* TryForward(Heap& collector, BaseObject* object)
    {
        return Heap::GetHeap().old().relocate_or_remap_object(object);
    }

    static BaseObject* WaitRoutedTipReady(
        Heap& collector, BaseObject* from, BaseObject* to, ZPage* forwarding)
    {
        (void)to;
        ZPage::RetainScope lease(forwarding);
        return ZGeneration::generation(forwarding->generation_id())->relocate().relocate_object_inner(from, forwarding);
    }

    static bool TryUpdateRefField(Heap& collector, BaseObject* obj, RefField<>& field, BaseObject*& newRef)
    {
        return ZBarrier::TryUpdateRefField(obj, field, newRef);
    }

    static BaseObject* ProductRelocateOrRemap(
        Heap& collector, BaseObject* from, ZGenerationId generation)
    {
        using ProductFn = BaseObject* (*)(Heap*, BaseObject*, ZGenerationId);
        void* handle = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
        GC_EXPECT_TRUE(handle != nullptr);
        void* symbol = handle == nullptr ? nullptr : dlsym(
            handle,
            "_ZN12MapleRuntime4Heap24relocate_or_remap_objectEPNS_10BaseObjectENS_13ZGenerationIdE");
        GC_EXPECT_TRUE(symbol != nullptr);
        Dl_info info {};
        GC_EXPECT_TRUE(symbol != nullptr && dladdr(symbol, &info) != 0 && info.dli_fname != nullptr &&
                       std::strstr(info.dli_fname, "libcangjie-runtime.so") != nullptr);
        BaseObject* result = symbol == nullptr ? nullptr :
            reinterpret_cast<ProductFn>(symbol)(&Heap::GetHeap(), from, generation);
        if (handle != nullptr) {
            (void)dlclose(handle);
        }
        return result;
    }

    static BaseObject* ForwardExclusive(
        Heap& collector, BaseObject* from)
    {
        return ZRelocate::ForwardObjectExclusive(from);
    }

    static BaseObject* ForwardImpl(Heap& collector, BaseObject* from, ZPage* copyPage)
    {
        ZPage::RetainScope lease(copyPage);
        return lease.ok() ? ZGeneration::generation(copyPage->generation_id())->relocate().relocate_object_inner(from, copyPage) : nullptr;
    }

    static void RemapYoungRoots(Heap& collector) { ZRelocate::RemapYoungRoots(); }

    static void SeedValueRoots(Heap& collector, BaseObject* value)
    {
        {
            std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().resurrectExportMtx);
            Heap::GetHeap().cross_vm().resurrectedExportObjectes.clear();
            Heap::GetHeap().cross_vm().resurrectedExportObjectesForwardPhase.clear();
            Heap::GetHeap().cross_vm().resurrectedExportObjectes.insert(value);
            Heap::GetHeap().cross_vm().resurrectedExportObjectesForwardPhase.insert(value);
        }
        std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().cycleWorkStackMtx);
        Heap::GetHeap().cross_vm().cycleRefWorkStack.clear();
        Heap::GetHeap().cross_vm().cycleRefWorkStack[value].push_back(value);
    }

    static bool AllValueRootCarriersEqual(Heap& collector, BaseObject* value)
    {
        bool resurrected = false;
        {
            std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().resurrectExportMtx);
            resurrected = Heap::GetHeap().cross_vm().resurrectedExportObjectes.size() == 1 &&
                Heap::GetHeap().cross_vm().resurrectedExportObjectes.count(value) == 1 &&
                Heap::GetHeap().cross_vm().resurrectedExportObjectesForwardPhase.size() == 1 &&
                Heap::GetHeap().cross_vm().resurrectedExportObjectesForwardPhase.count(value) == 1;
        }
        std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().cycleWorkStackMtx);
        auto it = Heap::GetHeap().cross_vm().cycleRefWorkStack.find(value);
        return resurrected && Heap::GetHeap().cross_vm().cycleRefWorkStack.size() == 1 &&
            it != Heap::GetHeap().cross_vm().cycleRefWorkStack.end() && it->second.size() == 1 &&
            it->second.front() == value;
    }

    static bool BothResurrectionSetsEqual(Heap& collector, BaseObject* value)
    {
        std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().resurrectExportMtx);
        return Heap::GetHeap().cross_vm().resurrectedExportObjectes.size() == 1 &&
            Heap::GetHeap().cross_vm().resurrectedExportObjectes.count(value) == 1 &&
            Heap::GetHeap().cross_vm().resurrectedExportObjectesForwardPhase.size() == 1 &&
            Heap::GetHeap().cross_vm().resurrectedExportObjectesForwardPhase.count(value) == 1;
    }

    static std::vector<BaseObject*> VisitMinorValueRoots(Heap& collector)
    {
        std::vector<BaseObject*> visited;
        Heap::GetHeap().cross_vm().VisitMinorValueRoots([&visited](BaseObject* value) { visited.push_back(value); });
        return visited;
    }

    // The product old-roots task routes these current objects through ZBarrier::Mark
    // (zMark.cpp mark_old_roots); observe the same visitor output.
    static std::vector<BaseObject*> EnumMajorValueRoots(Heap& collector)
    {
        std::vector<BaseObject*> visited;
        Heap::GetHeap().cross_vm().VisitSurrectedExportRoots([&](BaseObject* object) { visited.push_back(object); });
        std::reverse(visited.begin(), visited.end());
        return visited;
    }

    static void RunLateValueRootRekey(Heap& collector)
    {
        Heap::GetHeap().cross_vm().PreforwardDiscoveredExternObjects(Generation::Old);
        Heap::GetHeap().cross_vm().PreforwardAllResurrectExportFromObjects(Generation::Old);
    }
};

// The four delivery fixtures enter the same private product methods that their
// runtime callers use.  Friendship is enabled only for MRT_TESTABLE_INTERNALS;
// it adds no product export, branch, or runtime state.
struct LoadHealDeliveryTestAccess {
    struct RemsetConsumeResult {
        size_t work;
        size_t consumed;
    };

    static void PublishColours(Heap& collector) { ZGlobalsPointers::initialize(); }

    static uintptr_t DoubleBadColour(const Heap& collector)
    {
        return ZPointerRemappedMask & ~ZPointerRemappedYoungMask &
            ~ZPointerRemappedOldMask;
    }

    static void RemapYoungRoots(Heap& collector) { ZRelocate::RemapYoungRoots(); }

    static void FlipYoungRelocateStart(Heap& collector)
    {
        ZGlobalsPointers::flip_young_relocate_start();
    }

    static void FlipOldRelocateStart(Heap& collector)
    {
        ZGlobalsPointers::flip_old_relocate_start();
    }

    static RemsetConsumeResult ConsumeRemembered(Heap& collector,
                                                  const std::unordered_set<MAddress>& previous,
                                                  BaseObject* currentMinorRoot)
    {
        // This synthetic relocation fixture must bind the same collector
        // used by the global product barrier and provide its mark domain.
        // Full GC/phase production is separately covered by the managed P2 test.
        RelocationReceiptTest::BindCollector(&collector);
        auto& young = Heap::GetHeap().young();
        if (young.Workers() == nullptr) young.InitializeWorkers(1);
        Heap::GetHeap().young().Mark().BindWorkers(Heap::GetHeap().young().Workers());
        Heap::GetHeap().young().Mark().Start();
        GC_EXPECT_TRUE(Heap::GetHeap().young().Mark().Stripes().IsEmpty());
        young.PublishPhase(ZGenerationPhase::Mark);
        ZGlobalsPointers::flip_young_mark_start();
        WorkStack workStack = WorkStack{};
        std::unordered_set<MAddress> reachableSlots;
        std::unordered_set<MAddress> weakSlots;
        std::unordered_set<BaseObject*> currentMinorRoots;
        std::unordered_set<MAddress> consumed;
        RemsetScanStats stats;
        stats.recorded = previous.size();
        if (currentMinorRoot != nullptr) {
            currentMinorRoots.insert(currentMinorRoot);
        }
        auto& domain = *Heap::GetHeap().young().MarkPtr();
        auto& stacks = domain.Stacks();
        const size_t work = stacks.Population();
        for (size_t stripe = 0; stripe < domain.Stripes().NStripes(); ++stripe) {
            if (auto* stack = stacks.StealLocal(stripe)) MarkStripeStack::Destroy(stack);
        }
        RelocationReceiptTest::BindCollector(nullptr);
        return RemsetConsumeResult { work, consumed.size() };
    }
};

} // namespace MapleRuntime

namespace {

struct PageWaitEnterBarrier {
    static void Reset()
    {
        std::lock_guard<std::mutex> guard(mu);
        entered = false;
    }
    static void Hook(ZForwarding* forwarding)
    {
        GC_EXPECT_TRUE(forwarding != nullptr);
        GC_EXPECT_TRUE(!forwarding->is_done());
        std::lock_guard<std::mutex> guard(mu);
        entered = true;
        cv.notify_all();
    }
    static void WaitEntered()
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, []() { return entered; });
    }
    static std::mutex mu;
    static std::condition_variable cv;
    static bool entered;
};
std::mutex PageWaitEnterBarrier::mu;
std::condition_variable PageWaitEnterBarrier::cv;
bool PageWaitEnterBarrier::entered = false;

uintptr_t OneLoadBadRemap()
{
    const uintptr_t bad = static_cast<uintptr_t>(::g_cjLoadBadMask) & ZPointerRemappedMask;
    GC_EXPECT_TRUE(bad != 0);
    return bad & (~bad + 1);
}

class ResolveBarrier final {
public:
    ResolveBarrier() = default;
    ResolveBarrier(Heap&, RememberedSet&) {}

    BaseObject* Resolve(BaseObject* from) const
    {
        RefField<> field(StoreGoodPointer(from));
        ZGlobalsPointers::flip_old_relocate_start();
        BaseObject* result = ZBarrier::ReadReference(nullptr, field);
        ZGlobalsPointers::flip_old_relocate_start();
        return result;
    }
};

void EnsureDeliveryRuntime();

GcHeapFixture& ProductFixture()
{
    EnsureDeliveryRuntime();
    static GcHeapFixture fixture;
    RelocationReceiptTest::BindCollector(nullptr);
    static const bool initialized = InitFwdTables();
    // CompactRegion now carries remembered bits with an in-place copy.  This
    // independent product-test process does not run Heap::Init, so initialize
    // the Heap-owned remembered set alongside its forwarding table.
    static const bool rememberedInitialized = [&]() {
        return true;
    }();
    GC_EXPECT_TRUE(initialized);
    GC_EXPECT_TRUE(rememberedInitialized);
    return fixture;
}

class LoadHealDeliveryRuntime final : public Runtime {
public:
    static void Ensure()
    {
        static LoadHealDeliveryRuntime runtimeContainer;
        (void)runtimeContainer;
    }

    RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
    void SetGCThreshold(uint64_t) override {}

private:
    LoadHealDeliveryRuntime()
    {
        runtime = this;
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        manager.Init();
        const ConcurrencyParam concurrencyParam = { 1024, 64, 1 };
        concurrency.Init(concurrencyParam);
    }

    MutatorManager manager;
    Concurrency concurrency;
};

void EnsureDeliveryRuntime()
{
    static const int initialized = CJ_ScheduleManagerInit();
    GC_EXPECT_EQ(initialized, 0);
    LoadHealDeliveryRuntime::Ensure();
}

struct DeliveryReferenceArrayTypes {
    DeliveryReferenceArrayTypes()
    {
        std::memset(componentStorage, 0, sizeof(componentStorage));
        component = reinterpret_cast<TypeInfo*>(componentStorage);
        component->SetType(TypeKind::TYPE_KIND_CLASS);
        component->SetInstanceSize(sizeof(void*));

        std::memset(arrayStorage, 0, sizeof(arrayStorage));
        array = reinterpret_cast<TypeInfo*>(arrayStorage);
        array->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        array->SetComponentTypeInfo(component);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(this), sizeof(*this));
    }

    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)];
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)];
    TypeInfo* component = nullptr;
    TypeInfo* array = nullptr;
};

DeliveryReferenceArrayTypes& GetDeliveryReferenceArrayTypes()
{
    static DeliveryReferenceArrayTypes types;
    return types;
}

void PinOwnerGeneration(ZPage* region, Generation gen)
{
    region->reset(gen == Generation::Young ? PageAge::eden : PageAge::old);
}

void PublishGenerationMarkComplete(Generation gen)
{
    Heap::GetHeap().PublishGenerationPhase(
        gen == Generation::Old ? ZGenerationId::old : ZGenerationId::young, ZGenerationPhase::MarkComplete);
}

ZPage* ResetDeliveryUnit(GcHeapFixture& fx, size_t index)
{
    index += ZPage::GranuleIndex(fx.heapStart);
    ZPage* previous = Heap::page(ZPage::GranuleAddress(index));
    if (previous != nullptr) {
        if (previous->IsYoungRegion()) {
            previous->reset(PageAge::old);
        }
    }
    if (previous != nullptr && Heap::page(previous->GetRegionStart()) != nullptr) {
        ZPage::RetirePage(previous, []() {});
    }
    ZPage* region = ZPage::InitRegion(index, (1) * ZGranuleSize, ZPageType::small);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionAllocPtr(region->GetRegionStart());
    // ZHeap::alloc_page (zHeap.cpp:253-257) publishes before any page lookup
    // or ZGenerationYoung::flip_promote replaces this descriptor.
    PublishAllocatedPage(region);
    (void)fx;
    return region;
}

// ZObjectAllocator::PerAge::alloc_small_object uses a shared page, not a TLAB.
// Seed its ordinary allocation input for this synthetic heap. All CPU slots
// use the same page so migration cannot change the fixture's allocation input.
class DeliverySharedPageScope {
public:
    explicit DeliverySharedPageScope(ZPage* page)
        : manager(static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager())
    {
        // zObjectAllocator.hpp:41 ZPerCPU<ZPage*>: every CPU slot names the page.
        auto& allocator = *Heap::GetHeap().object_allocator().allocator(PageAge::old);
        ZPerCPUIterator<ZPage*> slots(&allocator.sharedSmallPage);
        for (ZPage** slot; slots.next(&slot);) {
            previous.push_back(__atomic_exchange_n(slot, page, __ATOMIC_ACQ_REL));
        }
    }
    ~DeliverySharedPageScope()
    {
        auto& allocator = *Heap::GetHeap().object_allocator().allocator(PageAge::old);
        for (uint32_t cpu = 0; cpu < previous.size(); ++cpu) {
            allocator.sharedSmallPage.set(previous[cpu], cpu);
        }
    }
private:
    RegionManager& manager;
    std::vector<ZPage*> previous;
};

ZLiveMap* PrepareForwardable(GcHeapFixture& fx, ZPage* region, MAddress liveObject)
{
    if (region->IsAllocating()) GcHeapFixture::AdvanceGeneration(region->GetOwnerGeneration());
    for (MAddress address = region->GetRegionStart(); address < liveObject;) {
        BaseObject* prefix = fx.PlaceObject(address);
        address += prefix->GetSize();
        GC_EXPECT_TRUE(address <= liveObject);
    }
    const Generation generation = region->GetOwnerGeneration();
    // The page owns its livemap (zPage.cpp:42); mark the live object into it.
    ZLiveMap* live = &region->livemap();
    GC_EXPECT_TRUE(live != nullptr);
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(region, reinterpret_cast<BaseObject*>(liveObject)));
    // The product freezes the selected set before publishing any page view.
    if (generation_forwarding_table(generation).get(region->GetRegionStart()) == nullptr) {
        GC_EXPECT_TRUE(BeginForwardingArena(generation, { region }));
    }
    if (generation == Generation::Young) {
        RelocationReceiptTest::PrepareProductPage<Generation::Young>(region);
    } else {
        RelocationReceiptTest::PrepareProductPage<Generation::Old>(region);
    }
    // This synthetic fixture leaves an unmaterialized allocation prefix.
    // Record the known object start explicitly; production freezes a dense
    // allocation walk inside PrepareForwardableRegion.
    return live;
}

void DestroyAfterGhostCleared(ZPage* region, const char* why)
{
    if (region != nullptr && region->IsYoungRegion() && false) {
            }
    PublishGenerationMarkComplete(Generation::Young);
    PublishGenerationMarkComplete(Generation::Old);
    (void)why;
    Heap::GetHeap().GetZGeneration(region->GetOwnerGeneration()).reset_relocation_set();
}

struct LateBackfillState {
    ZPage* region;
    ZPage* destination;
    BaseObject* from;
    BaseObject* to;
    ZLiveMap* live;
    Generation generation;
};

LateBackfillState PrepareLateBackfill(GcHeapFixture& fx, Heap& collector,
                                    Generation generation = Generation::Old, bool publishMapping = true)
{
    ZPage* region = ResetDeliveryUnit(fx, 5);
    ZPage* destination = ResetDeliveryUnit(fx, 2);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    if (generation == Generation::Young) region->reset(PageAge::eden);

    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    BaseObject* to = fx.PlaceObject(destination->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(to) + to->GetSize());
    RelocationReceiptTest::BindCollector(&collector);
    ZLiveMap* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));

    if (publishMapping) {
        const auto publication = forwarding_for_page(region, reinterpret_cast<MAddress>(from));
        // zRelocate.cpp:369: relocation to a fresh page is a disjoint copy.
        ZUtils::object_copy_disjoint(to_zaddress(reinterpret_cast<uintptr_t>(from)),
                                     to_zaddress(reinterpret_cast<uintptr_t>(to)), from->GetSize());
        GC_EXPECT_EQ(publication->insert(reinterpret_cast<MAddress>(from),
                                                   reinterpret_cast<MAddress>(to)), reinterpret_cast<MAddress>(to));
    }
    region->MarkForwardingDone();
    from->SetStateCode(ObjectState::FORWARDED);
    ZForwarding* table = generation_forwarding_table(generation).get(reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(table != nullptr);
    return LateBackfillState{ region, destination, from, to, live,
                              region->GetOwnerGeneration() };
}

void CleanupLateBackfill(GcHeapFixture& fx, LateBackfillState& state)
{
    // The scenario has consumed its receipt. Normalize the planted header
    // before asking product retirement to prove no source still needs it.
    state.from->SetStateCode(ObjectState::NORMAL);
    Heap::GetHeap().GetZGeneration(state.region->GetOwnerGeneration()).reset_relocation_set();
    (void)fx;
}

struct PartialCompactState {
    ZPage* region;
    ZPage* destination;
    BaseObject* liveObject;
    ZLiveMap* live;
    size_t objectSize;
};

PartialCompactState PreparePartialCompact(GcHeapFixture& fx, Heap& collector, bool exhaustDestination)
{
    ZPage* region = ResetDeliveryUnit(fx, 1);
    ZPage* destination = ResetDeliveryUnit(fx, 2);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);

    BaseObject* dead = fx.PlaceObject(region->GetRegionStart());
    const size_t objectSize = dead->GetSize();
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart() + objectSize);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(liveObject) + objectSize);
    destination->SetRegionAllocPtr(
        exhaustDestination ? destination->GetRegionEnd() : destination->GetRegionStart());
    RelocationReceiptTest::BindCollector(&collector);
    ZLiveMap* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(liveObject));
    return PartialCompactState{ region, destination, liveObject, live, objectSize };
}

void CleanupPartialCompact(GcHeapFixture& fx, PartialCompactState& state)
{
    Heap::GetHeap().GetZGeneration(state.region->GetOwnerGeneration()).reset_relocation_set();
    (void)fx;
}

RememberedSet& DeliveryRememberedSet(GcHeapFixture& fx)
{
    RememberedSet& remembered = HeapTestRemset();
    (void)fx;
    return remembered;
}

class DeliveryNoAllocBufferScope final {
public:
    DeliveryNoAllocBufferScope() : saved(ThreadLocal::GetAllocBuffer())
    {
        ThreadLocal::SetAllocBuffer(nullptr);
    }

    ~DeliveryNoAllocBufferScope()
    {
        ThreadLocal::SetAllocBuffer(saved);
    }

private:
    AllocBuffer* saved;
};

void EmptyBothRememberedFaces(RememberedSet& remembered)
{
    // ZRememberedSet owns two per-page faces. The old global DrainForMinor
    // adapter no longer consumes either bitmap.
    for (int face = 0; face < 2; ++face) {
        remembered.FlipForMinor();
        ZPageTableIterator pages(&Heap::page_table());
        for (ZPage* page; pages.next(&page);) {
            if (page->_remembered_set.is_initialized()) {
                page->_remembered_set.clear_previous();
            }
        }
    }
}

LateBackfillState PrepareValueRootForwarding(GcHeapFixture& fx, Heap& collector)
{
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ZForwarding* publication = forwarding_for_page(
        state.region, reinterpret_cast<MAddress>(state.from));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(publication->insert(reinterpret_cast<MAddress>(state.from),
                     reinterpret_cast<MAddress>(state.to)),
                 reinterpret_cast<MAddress>(state.to));
    return state;
}

bool AllVisitedEqual(const std::vector<BaseObject*>& visited, BaseObject* expected)
{
    if (visited.size() != 4) {
        return false;
    }
    for (BaseObject* value : visited) {
        if (value != expected) {
            return false;
        }
    }
    return true;
}

void CompleteValueRootCoverage()
{
    PublishGenerationMarkComplete(Generation::Young);
    PublishGenerationMarkComplete(Generation::Old);
    Heap::GetHeap().GetZGeneration(Generation::Young).reset_relocation_set();
    Heap::GetHeap().GetZGeneration(Generation::Old).reset_relocation_set();
}

} // namespace









// ZGC zRelocate.cpp:382-409: the mutator runtime entry itself must retain the
// forwarding page and perform the first copy before falling back to a worker.
// ResolveBarrier's completed-route case above returns at CopyCollector.h:448-454;
// call the exported product entry here so that this arm cannot borrow that fast
// return or a test-ELF inline definition.
namespace {
void ExerciseMutatorCopy(bool runtimeEntry)
{
    GcHeapFixture& fx = ProductFixture();
    ZPage* region = ResetDeliveryUnit(fx, 4);
    ZPage* destination = ResetDeliveryUnit(fx, 3);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    const size_t size = from->GetSize();
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + size);
    const MAddress expected = destination->GetRegionStart();
    destination->SetRegionAllocPtr(expected);
    Heap& collector = Heap::GetHeap();
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTest::BindCollector(&collector);
    (void)PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    DeliverySharedPageScope allocation(destination);
    BaseObject* result = runtimeEntry
        ? RelocationReceiptTest::ProductRelocateOrRemap(collector, from, region->generation_id())
        : RelocationReceiptTest::ForwardImpl(collector, from, region);
    const MAddress mapping = forwarding_find(Generation::Old, reinterpret_cast<MAddress>(from));
    std::fprintf(stderr, "MUTATOR_COPY_ASSERT_EXECUTED runtime=%d result=%zx mapping=%zx expected=%zx\n",
                 runtimeEntry, reinterpret_cast<MAddress>(result), mapping, expected);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(result), expected);
    GC_EXPECT_EQ(mapping, expected);
    GC_EXPECT_TRUE(result->GetTypeInfo() == fx.typeInfo);
    GC_EXPECT_TRUE(from->IsForwarded());
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTest::BindCollector(nullptr);
    Heap::GetHeap().GetZGeneration(Generation::Old).reset_relocation_set();
    }
}






// after the product lookup returns, then prove the record still describes the
// carrier inputs that selected Unavailable rather than those later faces.






// zRelocationSet.cpp:91-96 and zRelocate.cpp:1013-1047: retiring the old
// forwarding generation and installing the next one must not leave an object
// header claiming FORWARDED after the receipt that justified it is gone.

// Second family-8 path: a ROUTED page can retain a prior from->to receipt after
// raw-pin clears ghost and the next generation installs an empty active table.
// Retirement must preserve that receipt; active miss is not identity evidence.

// zGeneration.cpp:276-285 resets the old relocation set only after remap has
// consumed every source reference. A residual FORWARDED source proves that the
// port has not reached that state: keep its old receipt until a newer active
// generation publishes a successor instead of inferring identity at retirement.









#if defined(__linux__)
template <typename Fn>
void ExpectRootAbortAt(const char* siteSubstr, Fn&& fn)
{
    int pipefd[2];
    GC_EXPECT_EQ(pipe(pipefd), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        close(pipefd[0]);
        (void)dup2(pipefd[1], STDERR_FILENO);
        (void)dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        fn();
        _exit(0);
    }
    close(pipefd[1]);
    char buf[8192];
    size_t filled = 0;
    while (filled + 1 < sizeof(buf)) {
        const ssize_t n = read(pipefd[0], buf + filled, sizeof(buf) - 1 - filled);
        if (n <= 0) {
            break;
        }
        filled += static_cast<size_t>(n);
    }
    buf[filled] = '\0';
    close(pipefd[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    if (std::strstr(buf, siteSubstr) == nullptr) {
        std::fprintf(stderr, "ABORT_CAPTURE filled=%zu needle=%s\n---\n%s\n---\n",
                     filled, siteSubstr, buf);
    }
    GC_EXPECT_TRUE(std::strstr(buf, siteSubstr) != nullptr);
}

template <typename Fn>
void ExpectRootAbort(Fn&& fn)
{
    ExpectRootAbortAt("[LOADFC][fail-closed]", fn);
}

struct AbortCapture {
    int status;
    std::string output;
};

template <typename Fn>
AbortCapture CaptureAbort(Fn&& fn)
{
    int pipefd[2];
    GC_EXPECT_EQ(pipe(pipefd), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        close(pipefd[0]);
        (void)dup2(pipefd[1], STDERR_FILENO);
        (void)dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        fn();
        _exit(0);
    }
    close(pipefd[1]);
    std::string output;
    char buffer[1024];
    for (;;) {
        const ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) {
            output.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            break;
        }
        GC_EXPECT_EQ(errno, EINTR);
    }
    close(pipefd[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    return AbortCapture{ status, std::move(output) };
}



// The receipt, retirement and lookup all belong to the linked product SO.
// Save the expected identity from the actual publisher before retiring it;
// no LookupResult is constructed or passed to a product consumer by this test.
// ZGC zGeneration.inline.hpp:131-140: no forwarding leaves the address unchanged.
GC_OTHER_VM_TEST(FindToPublicState, NotManagedIsObservable)
{
    BaseObject* const result = Heap::GetHeap().old().relocate_or_remap_object(nullptr);
    GC_EXPECT_TRUE(result == nullptr);
}

RefField<>* gIncomingDestination = nullptr;
uintptr_t gIncomingDestinationExpected = 0;

void IncomingAbortWitness(int)
{
    const uintptr_t observed = gIncomingDestination == nullptr
        ? static_cast<uintptr_t>(-1)
        : raw(gIncomingDestination->GetFieldValue());
    static constexpr char kUnchanged[] = "INCOMING_DESTINATION_UNCHANGED\n";
    static constexpr char kChanged[] = "INCOMING_DESTINATION_CHANGED\n";
    if (observed == gIncomingDestinationExpected) {
        (void)write(STDERR_FILENO, kUnchanged, sizeof(kUnchanged) - 1);
        _exit(86);
    }
    (void)write(STDERR_FILENO, kChanged, sizeof(kChanged) - 1);
    _exit(87);
}





// ZBarrier::is_good_or_null_fast_path does not send a load-good to-version
// through the from-side slow path (zBarrier.inline.hpp:294-343).  Reproduce the
// NW256 identity with two retired carriers: the source carrier retains an
// explicit from->to receipt, while the destination carrier has no receipt for
// the same numerical to-address.  Once the destination is no longer a current
// from range, TRACE incoming must keep the to-address and perform no lookup.
// The direct resolver arm is the positive control: a real current from-range
// member still consumes its retired ArmedHit receipt when the already-to TRACE
// guard is cut.










// ProcessDerivedOop (oopMap.cpp:400-421): shared base, two distinct offsets.


// A managed frame is input data to the real mutator phase entry. Keep the
// descriptor in the loaded test image so the product metadata lifetime check
// can establish its identity; no stack scanner or resolver is replaced here.
#if defined(__x86_64__) && defined(__linux__)
namespace {
struct DerivedBaseMapImage {
    int32_t descriptorOffset;
    uint32_t pc[4];
    int32_t stackMapOffset;
    uint32_t descriptorRest[6];
    uint8_t bits[256];
};
DerivedBaseMapImage derivedBaseMapImage;

void InitializeDerivedBaseMap(bool tagged)
{
    auto& image = derivedBaseMapImage;
    std::memset(&image, 0, sizeof(image));
    image.descriptorOffset = reinterpret_cast<char*>(&image.stackMapOffset) -
        reinterpret_cast<char*>(&image.descriptorOffset);
    image.stackMapOffset = reinterpret_cast<char*>(image.bits) - reinterpret_cast<char*>(&image.stackMapOffset);
    ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(image.pc));
    size_t bit = 0;
    auto put = [&](uint32_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i, ++bit) {
            image.bits[bit / 8] |= ((value >> i) & 1u) << (bit % 8);
        }
    };
    auto var = [&](uint32_t value) {
        if (value <= 11) { put(value, 4); }
        else { put(12, 4); put(value, 8); }
    };
    var(0); var(2); var(0); // stack size, uncompressed + tagged format, no saved registers
    var(1); var(0); var(2); var(0); var(1); // one PC, reg/slot/line/derived index widths
    if (CangjieRuntime::stackGrowConfig == StackGrowConfig::STACK_GROW_ON) { var(0); var(0); }
    var(0); var(1); var(0); // tagged reg/slot widths, padding
    put(0, 32); put(tagged ? 0 : 1, 2); put(1, 1); put(tagged ? 1 : 0, 1);
    var(0); var(0); // empty register table
    var(2); var(8); var(1); // two slot rows: base at fp-24, derived at fp-16
    put(232, 8); put(1, 1); put(240, 8); put(1, 1);
    var(0); var(0); // empty line table
    var(1); put(2, 2); // derived row selects second slot row

}

void RunDerivedBaseProducer(bool tagged, bool moving = false, bool expectFailClosed = false,
                            bool unresolvedGhost = false)
{
    InitializeDerivedBaseMap(tagged);
    auto& image = derivedBaseMapImage;

    GcHeapFixture& fx = ProductFixture();
    Heap& collector = Heap::GetHeap();
    RelocationReceiptTest::BindCollector(&collector);
    LateBackfillState state {};
    if (unresolvedGhost) {
        state = PrepareLateBackfill(fx, collector);
        state.from->SetStateCode(ObjectState::NORMAL);
        state.region->MarkForwardingDone();
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
        auto& manager = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
        RelocationReceiptTest::ParkFrom(manager, state.region);
        auto& old = Heap::GetHeap().old();
        if (old.Workers() == nullptr) old.InitializeWorkers(1);
        old.Workers()->set_active_workers(1);
        old.Workers()->set_active();
        old.relocate().relocate(&old.relocation_set());
        old.Workers()->set_inactive();
        auto owner = forwarding_for_page(state.region);
        const MAddress fromAddr = reinterpret_cast<MAddress>(state.from);
        const MAddress produced = owner ? owner->find(fromAddr) : 0;
        const bool completed = owner && owner->is_done() && owner->ref_count().load() == 0;
        std::fprintf(stderr, "DERIVED_PAGE_TASK produced=%zx expected=%zx done_released=%d\n",
                     produced, reinterpret_cast<MAddress>(state.to), completed);
        GC_EXPECT_TRUE(completed);
        GC_EXPECT_EQ(produced, reinterpret_cast<MAddress>(state.to));
        ForwardingCursor cursor = 0;
        GC_EXPECT_TRUE(owner->find(owner->index(fromAddr), &cursor).populated());
        owner->entries()[cursor].store(0, std::memory_order_release);
    } else if (moving) {
        state = PrepareValueRootForwarding(fx, collector);
    }
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
    const bool usesState = moving || unresolvedGhost;
    const uintptr_t base = reinterpret_cast<uintptr_t>(usesState ? state.from : fx.obj0);
    const uintptr_t expected = reinterpret_cast<uintptr_t>(moving ? state.to : fx.obj0);
    uintptr_t frame[8] = {};
    frame[0] = base;
    frame[1] = base + 8;
    frame[2] = reinterpret_cast<uintptr_t>(image.pc) + 9;
    Mutator mutator;
    auto& context = mutator.GetUnwindContext();
    context.frameInfo.mFrame.SetIP(image.pc);
    context.frameInfo.mFrame.SetFA(reinterpret_cast<FrameAddress*>(&frame[3]));
    context.anchorFA = nullptr;
    std::fprintf(stderr, "DERIVED_BASE_INPUT tagged=%d moving=%d base=%zx derived=%zx\n",
                 tagged, moving, frame[0], frame[1]);
    if (expectFailClosed) {
        AbortCapture aborted = CaptureAbort([&]() {
            });
        std::fprintf(stderr, "DERIVED_BASE_FAILCLOSED status=%d\n%s", aborted.status, aborted.output.c_str());
        if (usesState) { CleanupLateBackfill(fx, state); }
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
        RelocationReceiptTest::BindCollector(nullptr);
        std::fprintf(stderr, "DERIVED_BASE_TARGET target_assertion executed=1 matched=%d\n",
                     aborted.output.find("should be forwarded from=") !=
                         std::string::npos);
        GC_EXPECT_TRUE(aborted.output.find("should be forwarded from=") !=
                       std::string::npos);
        GC_EXPECT_TRUE(WIFSIGNALED(aborted.status));
        GC_EXPECT_EQ(WTERMSIG(aborted.status), SIGABRT);
        return;
    }
    std::fprintf(stderr, "DERIVED_BASE_RESULT base=%zx derived=%zx expected=%zx\n", frame[0], frame[1], expected + 8);
    const bool baseCorrect = frame[0] == expected;
    const bool derivedCorrect = frame[1] == expected + 8;
    if (usesState) { CleanupLateBackfill(fx, state); }
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTest::BindCollector(nullptr);
    GC_EXPECT_TRUE(derivedCorrect);
    GC_EXPECT_TRUE(baseCorrect);
}
}




#endif



// A non-LookupUnavailable route may carry lookup-shaped fields from a caller,
// but with the snapshot validity bit cleared they must never be rendered as
// legal-looking zero values.




// SD forwarding consumer gate: a compacted destination can be classified as
// kAlreadyToStart by reverse geometry, but that classification is not a
// load-good receipt. Make the destination header FORWARDED and drive the
// product ResolveStoreValue entry; the only legal result is fail-closed.










// zRelocate.cpp:382-415 and zForwarding.inline.hpp:267-303: an inserted
// winner, including an in-place identity, is the result of subsequent lookups.
// The removed WaitRouted observation branches have no product counterpart;
// exercise their surviving address invariant without lookup-count scheduling.
static void CheckForwardingWinner(bool identity)
{
    GcHeapFixture& fx = ProductFixture();
    ZPage* region = ResetDeliveryUnit(fx, 4);
    GC_EXPECT_TRUE(region != nullptr);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    const MAddress fromAddr = reinterpret_cast<MAddress>(from);
    region->SetRegionAllocPtr(fromAddr + from->GetSize());
    BaseObject* winner = identity ? from : fx.obj1;
    BaseObject* loser = identity ? fx.obj1 : from;
    Heap& collector = Heap::GetHeap();
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTest::BindCollector(&collector);
    (void)PrepareForwardable(fx, region, fromAddr);
    GC_EXPECT_EQ(forwarding_find(Generation::Old, fromAddr), 0);
    GC_EXPECT_FALSE(region->IsForwardingDone());
    {
        auto publication = forwarding_for_page(region, fromAddr);
        GC_EXPECT_TRUE(static_cast<bool>(publication));
        GC_EXPECT_EQ(publication->insert(fromAddr,
                         reinterpret_cast<MAddress>(winner)), reinterpret_cast<MAddress>(winner));
        GC_EXPECT_EQ(publication->insert(fromAddr,
                         reinterpret_cast<MAddress>(loser)), reinterpret_cast<MAddress>(winner));
    }
    {
        ZPage::RetainScope lease(region);
        GC_EXPECT_TRUE(lease.ok());
        GC_EXPECT_TRUE(RelocationReceiptTest::WaitRoutedTipReady(
                           collector, from, nullptr, region) == winner);
        GC_EXPECT_EQ(forwarding_find(Generation::Old, fromAddr), reinterpret_cast<MAddress>(winner));
    }
    region->MarkForwardingDone();
    GC_EXPECT_TRUE(region->IsForwardingDone());
    GC_EXPECT_EQ(forwarding_find(Generation::Old, fromAddr), reinterpret_cast<MAddress>(winner));
    region->ReleaseForwarding();
    GC_EXPECT_FALSE(region->RetainForwarding());
    GC_EXPECT_EQ(forwarding_find(Generation::Old, fromAddr), reinterpret_cast<MAddress>(winner));

        RelocationReceiptTest::BindCollector(nullptr);
}





// zRelocate.cpp:412-415: completing a page is not a forwarding receipt.
// Preserve the old published-miss rejection invariant at the current product
// exit, without requiring the deleted WaitRouted diagnostic branch or fields.



#endif











#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)

#endif // MRT_PRODUCT_TESTABLE_INTERNALS

#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)

#endif // MRT_PRODUCT_TESTABLE_INTERNALS

// Product compact-request entry: the request is registered before compaction;
// CompactRegion itself copies the live second object, inserts its receipt, then
// zeroes that from slot.  The resolver must therefore answer the installed to,
// never the cleared from address.


// ClearEntries must seal an installed table and wait for the publication owner
// that crossed the copy boundary.  The owner inserts while clear is waiting;
// only after the owner releases may clear unlink and retire the table.

// zRelocationSet.cpp:191-197: clearing the table waits for outstanding table
// users independently of the source-page count. Observe drain admission and
// the held publication token before allowing the publisher to complete.

// zRelocate.cpp:362-372: Exclusive owns the before-copy Publication through
// CopyObject, receipt installation, queue publication and FORWARDED state.  Use
// the product allocator's real queue so no receipt is hand-fed by this test.


// ZGC zRelocate.cpp:1256-1279: the promoted page keeps the relocation-set
// livemap selected at registration, and discharge walks only that live set.


// zForwarding.cpp:55-84 / zRelocate.cpp:871-877: resetting the to-page
// allocation top must not retarget the from-page iteration view. The consumer
// keeps using the forwarding carrier until Dispel retires it.


// ZGC zRelocate.cpp:1256-1279: the promoted page keeps the relocation-set
// livemap selected at registration, and discharge walks only that live set.


// ZGC zRelocate.cpp:652-731,838-861: lift the old page face before reuse,
// move a field bit with its object, then prove the real minor consumer reaches
// the young target through the moved slot.


// ZRelocateWork::update_remset_promoted: young targets are remembered;
// old targets are remapped without adding a remembered bit.


// zRelocate.cpp:780-784,1241: resolve a young from-address to an old
// target and heal the field before omitting its remset entry.


// Direct semantic matrix for the current remembered face. The reference array
// is live, and its far field lies beyond the former 64-byte
// recovery window. ZGC still applies the load barrier because the current old
// page, rather than an object-level recovery guess, is the admission unit.


// ZGenerationOld::remap_young_roots, zGeneration.cpp:1509: enter through
// the real major driver; a registered runtime mutator owns the raw root.
void RunMajorRawRemap(bool promoted, bool managed, bool oldPending = false, bool fallback = false, unsigned nestedKind = 0)
{
    // #720 creates worker/stat threads with the heap. Construct it in the
    // captured child, rather than inheriting a preconstructed heap at fork.
    CreateStandaloneHeap(1024);
    ZStat::Initialize();
    GcHeapFixture& fx = ProductFixture();
    Heap& collector = Heap::GetHeap();
    RelocationReceiptTest::BindCollector(&collector);
    LoadHealDeliveryTestAccess::PublishColours(collector);
    LateBackfillState forwarding {};
    BaseObject* secondOld = nullptr;
    if (oldPending) {
        ZPage* region = ResetDeliveryUnit(fx, 5);
        BaseObject* dead = fx.PlaceObject(region->GetRegionStart());
        BaseObject* from = fx.PlaceObject(region->GetRegionStart() + dead->GetSize());
        region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
        ZLiveMap* live = &region->livemap();
        auto& regionManager = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
        RelocationReceiptTest::ParkFrom(regionManager, region);
        // ZGC selects a set only when packing can release a page. Two sparse
        // pages are input to the real selector; a single page is exempted.
        ZPage* second = ResetDeliveryUnit(fx, 4);
        secondOld = fx.PlaceObject(second->GetRegionStart());
        second->SetRegionAllocPtr(reinterpret_cast<MAddress>(secondOld) + secondOld->GetSize());
        RelocationReceiptTest::ParkFrom(regionManager, second);
        forwarding = {region, region, from, dead, live, Generation::Old};
    } else {
        forwarding = PrepareLateBackfill(fx, collector, Generation::Young);
    }
    std::unique_ptr<DeliverySharedPageScope> allocation;
    if (oldPending) {
        // Supply allocation capacity, as Heap::Init would. The product copier
        // still allocates, copies and publishes the destination itself.
        allocation = std::make_unique<DeliverySharedPageScope>(ResetDeliveryUnit(fx, 2));
    }
    if (promoted) {
        // The source table, not its current page generation, owns remapping.
        forwarding.region->reset(PageAge::old);
    }

    // ResetDeliveryUnit publishes each allocated page before returning it,
    // matching ZHeap::alloc_page (zHeap.cpp:253-257). Do not register it twice.
    MutatorManager& manager = MutatorManager::Instance();
    Mutator* mutator = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    const size_t frameMark = mutator->NativeFrameRootCount();
    alignas(16) uintptr_t nestedStorage[8] {};
    RootSlot* nestedField = nullptr;
    BaseObject* rootInput = forwarding.from;
    if (nestedKind != 0) {
        mutator->SetStackTopAddr(reinterpret_cast<uintptr_t>(nestedStorage));
        mutator->SetStackSize(sizeof(nestedStorage));
        rootInput = reinterpret_cast<BaseObject*>(&nestedStorage[2]);
        if (nestedKind == 1) rootInput->SetClassInfo(fx.typeInfo);
        nestedField = &RootSlotAt(static_cast<void*>(&nestedStorage[nestedKind == 1 ? 3 : 2]));
        StorePlain(*nestedField, from_object(forwarding.from));
    }
    const size_t rootMark = mutator->NativeFrameRootCount();
    // Exercise ordinary frame push/pop once to establish capacity for this
    // frame. No subsequently acquired slot address crosses vector growth.
    for (unsigned i = 0; i < 4; ++i) (void)mutator->AddNativeFrameRoot(nullptr);
    mutator->PopNativeFrameRootsTo(rootMark);
    ObjectRef* root = mutator->AddNativeFrameRoot(rootInput);
    if (secondOld != nullptr) (void)mutator->AddNativeFrameRoot(secondOld);
    ObjectRef* nullRoot = mutator->AddNativeFrameRoot(nullptr);
    static uintptr_t nonHeapStorage[2] = {};
    ObjectRef* nonHeapRoot = mutator->AddNativeFrameRoot(reinterpret_cast<BaseObject*>(nonHeapStorage));
    uintptr_t frame[8] = {};
#if defined(__x86_64__) && defined(__linux__)
    if (managed) {
        InitializeDerivedBaseMap(false);
        frame[0] = reinterpret_cast<uintptr_t>(forwarding.from);
        frame[1] = frame[0] + 8;
        frame[2] = reinterpret_cast<uintptr_t>(derivedBaseMapImage.pc) + 9;
        auto& context = mutator->GetUnwindContext();
        context.frameInfo.mFrame.SetIP(derivedBaseMapImage.pc);
        context.frameInfo.mFrame.SetFA(reinterpret_cast<FrameAddress*>(&frame[3]));
        context.anchorFA = nullptr;
        mutator->SetManagedContext(true);
        if (fallback) {
            // Missing stack bounds is the product's explicit legacy fallback
            // case. The valid frame metadata and heap roots remain unchanged.
            mutator->SetStackTopAddr(0);
        }
    }
#endif
    // The thread must predate the relocation color flip: its raw roots still
    // name the from-page, so its saved load-good mask must describe that epoch.
    if (!oldPending) LoadHealDeliveryTestAccess::FlipYoungRelocateStart(collector);
    Heap::GetHeap().GetZGeneration(ZGenerationId::young).InitializeWorkers(2);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).InitializeWorkers(2);
    // This fixture invokes the old body without the driver's young prelude.
    // Supply the product mark-start sequence event before publishing old roots.
    auto& oldCycle = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
    if (oldCycle.Snapshot().active) oldCycle.End();
    // Enter the actual old mark-start producer; a bare ZMark::Start only
    // initializes stacks and does not publish the generation's Mark phase.
    Heap::GetHeap().old().mark_start();
    {
        DriverLocker driver;
        ZDriver::RunGarbageCollection(1, GC_REASON_USER);
    }
    const uintptr_t expected = oldPending
        ? forwarding_find(Generation::Old, reinterpret_cast<uintptr_t>(forwarding.from))
        : reinterpret_cast<uintptr_t>(forwarding.to);
    const uintptr_t before = reinterpret_cast<uintptr_t>(forwarding.from);
    if (oldPending) {
        // ZGC zGeneration.cpp:1015-1071,1379-1404 does not eagerly heal raw
        // slots at old relocate-start. Check that the driver produced a new
        // target while the unprocessed native/derived roots retain old values.
        const bool deferred = raw(root->LoadPlain()) == before && expected != 0 && expected != before &&
            (!managed || (frame[0] == before && frame[1] == before + 8));
        std::fprintf(stderr, "OLD_PENDING_TARGET_ASSERT before=%zx root=%zx winner=%zx base=%zx derived=%zx deferred=%u\n",
            before, raw(root->LoadPlain()), expected, frame[0], frame[1], unsigned(deferred));
        GC_EXPECT_TRUE(deferred);
        // zUncoloredRoot.inline.hpp:35-64: the next real root processing
        // makes this very slot load-good and writes it back. No winner is
        // handed to this entry by the test.
        (void)mutator->GcPhaseEnum(false);
    }
    const uintptr_t observedRoot = nestedField == nullptr ? raw(root->LoadPlain()) : raw(nestedField->LoadPlain());
    const bool result = expected != 0 && (!oldPending || expected != before) &&
        (nestedField == nullptr ? observedRoot == expected :
            raw(root->LoadPlain()) == reinterpret_cast<uintptr_t>(rootInput) && observedRoot == expected) &&
        is_null(nullRoot->LoadPlain()) && raw(nonHeapRoot->LoadPlain()) == reinterpret_cast<uintptr_t>(nonHeapStorage) &&
        (!managed || (frame[0] == expected && frame[1] == expected + 8));
    std::fprintf(stderr, "RAW_REMAP_TARGET_ASSERT promoted=%u managed=%u before=%zx after=%zx "
        "expected=%zx base=%zx derived=%zx final_root=%zx result=%u\n", unsigned(promoted), unsigned(managed),
        before, observedRoot, expected, frame[0], frame[1], raw(root->LoadPlain()), unsigned(result));
    if (nestedKind != 0) {
        std::fprintf(stderr, "NESTED_REMAP_TARGET kind=%u observed=%zx expected=%zx result=%u\n",
                     nestedKind, raw(nestedField->LoadPlain()), expected, unsigned(result));
    }
    GC_EXPECT_TRUE(result);
    mutator->PopNativeFrameRootsTo(frameMark);
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
}
void CheckMajorRawRemap(bool promoted, bool managed, bool oldPending = false, bool fallback = false, unsigned nestedKind = 0)
{
    const AbortCapture outcome = CaptureAbort([&] { RunMajorRawRemap(promoted, managed, oldPending, fallback, nestedKind); });
    std::fprintf(stderr, "%s", outcome.output.c_str());
    const bool completed = WIFEXITED(outcome.status) && WEXITSTATUS(outcome.status) == 0 &&
        outcome.output.find("result=1") != std::string::npos;
    std::fprintf(stderr, "RAW_REMAP_OUTCOME_ASSERT promoted=%u managed=%u old=%u status=%d completed=%u\n",
        unsigned(promoted), unsigned(managed), unsigned(oldPending), outcome.status, unsigned(completed));
    GC_EXPECT_TRUE(completed);
}
GC_RUNTIME_OTHER_VM_TEST(RawRemapYoungProduct, MajorRemapsStackObjectField)
{
    CheckMajorRawRemap(false, false, false, false, 1);
}
GC_RUNTIME_OTHER_VM_TEST(RawRemapYoungProduct, MajorRemapsHeaderlessRecordField)
{
    CheckMajorRawRemap(false, false, false, false, 2);
}
GC_RUNTIME_OTHER_VM_TEST(RawRemapYoungProduct, MajorWatermarkConsumesYoungTable)
{
    CheckMajorRawRemap(false, false);
}
GC_RUNTIME_OTHER_VM_TEST(RawRemapYoungProduct, MajorWatermarkConsumesPromotedYoungSource)
{
    CheckMajorRawRemap(true, false);
}
GC_RUNTIME_OTHER_VM_TEST(RawRemapYoungProduct, MajorKeepsOldRawRootUntilNextRootScan)
{
    CheckMajorRawRemap(false, false, true);
}
#if defined(__x86_64__) && defined(__linux__)
GC_RUNTIME_OTHER_VM_TEST(RawRemapYoungProduct, MajorWatermarkRemapsDerivedYoungSource)
{
    CheckMajorRawRemap(false, true);
}
GC_RUNTIME_OTHER_VM_TEST(RawRemapYoungProduct, MajorWatermarkRemapsDerivedPromotedSource)
{
    CheckMajorRawRemap(true, true);
}
GC_RUNTIME_OTHER_VM_TEST(RawRemapYoungProduct, MajorKeepsOldDerivedRootUntilNextRootScan)
{
    CheckMajorRawRemap(false, true, true);
}
GC_RUNTIME_OTHER_VM_TEST(RawRemapYoungProduct, MajorFallbackRemapsDerivedPromotedSource)
{
    CheckMajorRawRemap(true, true, false, true);
}
GC_RUNTIME_OTHER_VM_TEST(RawRemapYoungProduct, MajorFallbackKeepsOldRootUntilNextRootScan)
{
    CheckMajorRawRemap(false, true, true, true);
}
#endif

#include "b09_runtime_fixture.hpp"

static void CheckCompactIncoming(bool overlapping, bool external = false, bool major = false, bool flipYoung = false, bool rootBeforeCompact = false, bool exportEntry = false)
{
    B09RuntimeFixture runtime;
    GcHeapFixture& fx = ProductFixture();
    Heap& collector = Heap::GetHeap();
    auto* region = fx.region0;
    const MAddress start = region->GetRegionStart();
    auto* dead = fx.PlaceObject(start);
    const size_t size = dead->GetSize();
    auto* first = fx.PlaceObject(start + size);
    auto* second = fx.PlaceObject(start + 2 * size);
    region->SetRegionAllocPtr(start + 3 * size);
    fx.region1->SetRegionAllocPtr(fx.region1->GetRegionEnd());
    RelocationReceiptTest::BindCollector(&collector);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    (void)PrepareForwardable(fx, region, reinterpret_cast<MAddress>(first));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(region, second));
    RegionManager manager;
    RelocationReceiptTest::ParkFrom(manager, region);
    auto& queue = generation_relocate_queue(Generation::Old);
    queue.BeginWorkers(1);
    const auto request = queue.Add(region, reinterpret_cast<MAddress>(second));
    GC_EXPECT_TRUE(request.accepted);
    if (rootBeforeCompact) {
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
        Heap::GetHeap().cross_vm().ResurrectExportObject(second);
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
        Heap::GetHeap().cross_vm().ResurrectExportObject(second);
        LoadHealDeliveryTestAccess::FlipOldRelocateStart(collector);
    }
    manager.CompactRegion(region);
    region->MarkForwardingDone();
    (void)queue.Wait(request.forwarding);
    auto* forwarding = request.forwarding;
    const MAddress firstTo = forwarding->find(start + size);
    const MAddress secondTo = forwarding->find(start + 2 * size);
    std::fprintf(stderr, "B09_OVERLAP_PRECONDITION size=%zu first_delta=%zu second_delta=%zu\n", size, firstTo-start, secondTo-start);
    GC_EXPECT_EQ(firstTo, start);
    GC_EXPECT_EQ(secondTo, start + size);
    auto* current = external ? fx.PlaceObject(fx.region1->GetRegionStart()) : reinterpret_cast<BaseObject*>(overlapping ? secondTo : firstTo);
    GC_EXPECT_TRUE(current->IsValidObject());
    if (!rootBeforeCompact) {
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
        if (exportEntry) {
            const U64 handle = Heap::GetHeap().RegisterExportRoot(current);
            Heap::GetHeap().CrossAccessBarrier(handle);
            Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
            Heap::GetHeap().CrossAccessBarrier(handle);
            Heap::GetHeap().RemoveExportObject(handle);
        } else {
            Heap::GetHeap().cross_vm().ResurrectExportObject(current);
            Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
            Heap::GetHeap().cross_vm().ResurrectExportObject(current);
        }
    }
    const bool identity = RelocationReceiptTest::BothResurrectionSetsEqual(
        collector, rootBeforeCompact ? second : current);
    std::fprintf(stderr, "B09_OVERLAP_TARGET_ASSERT current_identity=%d\n", identity);
    GC_EXPECT_TRUE(identity);
    if (flipYoung) {
        LoadHealDeliveryTestAccess::FlipYoungRelocateStart(collector);
    }
    const auto visited = major ? RelocationReceiptTest::EnumMajorValueRoots(collector)
                               : RelocationReceiptTest::VisitMinorValueRoots(collector);
    const bool consumerIdentity = visited.size() == 2 &&
        std::all_of(visited.begin(), visited.end(), [current](BaseObject* p) { return p == current; }) &&
        RelocationReceiptTest::BothResurrectionSetsEqual(collector, current);
    std::fprintf(stderr, "B09_CONSUMER_TARGET_ASSERT mode=%s count=%zu identity=%d\n",
                 major ? "major" : "minor", visited.size(), consumerIdentity);
    GC_EXPECT_TRUE(consumerIdentity);
}
























// ZPage::clone_for_promotion (zPage.cpp:64-72) + flip_promote (zGeneration.cpp:941-948):
// page-table identity becomes the cloned old page; the young from-page remains
// for remset/barriers until descriptor retire.
GC_TEST(PageGeneration579, PromotionAndCarrierRouting)
{
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    RelocationReceiptTest::BindCollector(nullptr);
    GcHeapFixture fixture;
    auto* region = fixture.region0;
    fixture.obj0 = fixture.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(region->GetRegionStart() + fixture.obj0->GetSize());
    region->reset(PageAge::eden);
    const MAddress from = reinterpret_cast<MAddress>(fixture.obj0);
    GC_EXPECT_TRUE(Heap::GetHeap().ObjectGeneration(fixture.obj0) == Generation::Young);
    GC_EXPECT_TRUE(region->generation_id() == ZGenerationId::young);
    ZPage* promoted = region->clone_for_promotion();
    ZGeneration::young()->flip_promote(region, promoted);
    fixture.region0 = promoted;
    std::fprintf(stderr, "PAGE579 promotion table=%p from=%p to=%p\n",
                 static_cast<void*>(Heap::page(from)), static_cast<void*>(region),
                 static_cast<void*>(promoted));
    GC_EXPECT_TRUE(Heap::page(from) == promoted);
    std::fprintf(stderr, "PAGE579 expect-table-ok\n");
    GC_EXPECT_TRUE(promoted->generation_id() == ZGenerationId::old);
    GC_EXPECT_TRUE(region->generation_id() == ZGenerationId::young);
    std::fprintf(stderr, "PAGE579 expect-ids-ok\n");
    fixture.region0 = promoted;
    std::fprintf(stderr, "PAGE579 body-done\n");
}

// ZFlipAgePagesTask promotion fork (zRelocate.cpp:1347-1363): the from_page's
// RegionList slot is handed to the promoted clone at the fork, so the list
// keeps exactly one member and page_table maps the range to the clone. The
// steps below are exactly the fork's promotion arm in order; the standalone
// fixture cannot run ZWorkers::run to reach the task itself (the young
// relocation set's _promotion_lock is wedged from process start, observed
// 0919), so this drives the same product calls sequentially.
GC_TEST(PageGeneration579, FlipAgePagesHandsRegionListSlotToPromotedPage)
{
    RelocationReceiptTest::BindCollector(nullptr);
    GcHeapFixture fixture;
    auto* region = fixture.region0;
    fixture.obj0 = fixture.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(region->GetRegionStart() + fixture.obj0->GetSize());
    region->reset(PageAge::eden);
    ZPage* promoted = region->clone_for_promotion();
    promoted->reset_livemap();
    ZGeneration::young()->flip_promote(region, promoted);
    GC_EXPECT_TRUE(Heap::page(region->GetRegionStart()) == promoted);
    GC_EXPECT_TRUE(promoted->generation_id() == ZGenerationId::old);
    fixture.region0 = promoted;
    ZPage::RetireDescriptor(region);
}

GC_TEST(PageGeneration579, ResetAndReuseCurrentGeneration)
{
    RelocationReceiptTest::BindCollector(nullptr);
    GcHeapFixture fixture;
    auto* region = fixture.region0;
    auto& collector = Heap::GetHeap();
    for (uint8_t young : {1, 0, 1, 0}) {
        region->reset(young ? PageAge::eden : PageAge::old);
        const Generation expected = young ? Generation::Young : Generation::Old;
        const ZGenerationId expectedId = young ? ZGenerationId::young : ZGenerationId::old;
        const Generation current = Heap::GetHeap().ObjectGeneration(fixture.obj0);
        std::fprintf(stderr, "PAGE579 reset young=%u current=%u id=%u\n", young,
                     static_cast<unsigned>(current), static_cast<unsigned>(region->generation_id()));
        GC_EXPECT_TRUE(current == expected);
        GC_EXPECT_TRUE(region->generation_id() == expectedId);
    }
    const auto oldLife = region->GetRegionLifeId();
    RegionLifeId retiredLife = oldLife;
    ZPage::RetirePage(region, [region, &retiredLife]() {
        region->RetirePageMemory();
        retiredLife = region->GetRegionLifeId();
    });
    // The scratch life counter belongs to one descriptor, not its address.
    GC_EXPECT_TRUE(retiredLife != oldLife);
    // ZPage::reset_seqnum/reset (zPage.cpp:90-112): a newly constructed page
    // snapshots the current generation, even when its address is reused.
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    region = ZPage::InitRegion(ZPage::GranuleIndex(fixture.heapStart), (1) * ZGranuleSize, ZPageType::small);
    PublishAllocatedPage(region);
    fixture.region0 = region;
    GC_EXPECT_EQ(region->seqnum(), ZGeneration::old()->seqnum());
    GC_EXPECT_TRUE(region->generation_id() == ZGenerationId::old);
    GC_EXPECT_TRUE(Heap::GetHeap().ObjectGeneration(fixture.obj0) == Generation::Old);
}

namespace {
// A completed forwarding may still cover an address that is already load-good.
// The product field consumer must use its colour, not table membership, to
// decide whether the address is from-space (ZGC zBarrier.inline.hpp:294-305).
void CheckMinorFieldColour(bool stale)
{
    GcHeapFixture fx;
    fx.region0->reset(PageAge::eden);
    fx.obj0 = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(fx.obj0) + fx.obj0->GetSize());
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fx.region0, fx.obj0));
    fx.InstallPageOwner(fx.region0);
    ZForwarding* forwarding = forwarding_for_page(fx.region0);
    const MAddress from = reinterpret_cast<MAddress>(fx.obj0);
    const MAddress to = reinterpret_cast<MAddress>(fx.obj1);
    GC_EXPECT_EQ(forwarding->insert(from, to), to);
    forwarding->release_page();
    forwarding->mark_done();
    zpointer bits = ZAddress::store_good(from_object(fx.obj0));
    if (stale) {
        // Preserve all other colour families; the previous young remap bit
        // directs make_load_good to the young generation's forwarding table.
        bits = ColouredPointer(fx.obj0, ZPointerRemappedOldMask & ~ZPointerRemappedYoungMask);
    }
    RefField<> field(bits);
    (void)ZRelocate::FixMinorEvacuatedSlot(field, nullptr, nullptr);
    const MAddress actual = untype(field.GetTargetObject());
    const MAddress expected = stale ? to : from;
    std::fprintf(stderr, "DETAIL minor_field_colour stale=%u actual=%#zx expected=%#zx\n",
                 static_cast<unsigned>(stale), actual, expected);
    GC_EXPECT_EQ(actual, expected);
}

GC_TEST(RelocateField782, LoadGoodAddressIsNotForwardedTwice)
{
    CheckMinorFieldColour(false);
}

GC_TEST(RelocateField782, LoadBadAddressConsumesForwarding)
{
    CheckMinorFieldColour(true);
}

}


// ZGC zRelocate.cpp:392-410: start with no forwarding entry. Unlike the
// colour tests, these cases require the product to publish the first entry.
GC_TEST(RelocateMiss782, RetainedPageCopiesAndPublishes)
{
    ExerciseMutatorCopy(true);
}

namespace {
void ExerciseRelocationWait782(bool claimedPage)
{
    GcHeapFixture& fx = ProductFixture();
    ZPage* source = ResetDeliveryUnit(fx, 4);
    ZPage* destination = ResetDeliveryUnit(fx, 3);
    BaseObject* from = fx.PlaceObject(source->GetRegionStart());
    source->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    destination->SetRegionAllocPtr(destination->GetRegionStart());
    Heap& heap = Heap::GetHeap();
    RelocationReceiptTest::BindCollector(&heap);
    auto& generation = heap.GetZGeneration(ZGenerationId::old);
    generation.set_phase(ZGenerationPhase::Relocate);
    PrepareForwardable(fx, source, reinterpret_cast<MAddress>(from));
    ZForwarding* forwarding = forwarding_for_page(source);
    GC_EXPECT_EQ(forwarding->find(reinterpret_cast<MAddress>(from)), 0U);
    DeliverySharedPageScope allocation(destination);
    std::vector<ZPage*> occupied;
    if (claimedPage) {
        // Ordinary forwarding lifecycle input: a worker has claimed the page
        // for in-place relocation before the mutator's first lookup.
        forwarding->in_place_relocation_claim_page();
    } else {
        // Exhaust actual non-blocking page capacity, with no allocation hook.
        destination->SetRegionAllocPtr(destination->GetRegionEnd());
        ZAllocationFlags flags;
        flags.set_non_blocking();
        while (ZPage* page = Heap::alloc_page(ZPageSizeSmall, ZPageType::small,
                   false, false, false, PageAge::old, flags)) {
            occupied.push_back(page);
        }
    }
    ZRelocateQueue* queue = generation.relocate().queue();
    std::atomic<bool> returned{false};
    BaseObject* result = nullptr;
    std::thread mutator([&] {
        result = RelocationReceiptTest::ProductRelocateOrRemap(heap, from, ZGenerationId::old);
        returned.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (queue->PendingCount() == 0 && !returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool queued = queue->PendingCount() != 0;
    const bool blocked = !returned.load(std::memory_order_acquire);
    const MAddress before = forwarding->find(reinterpret_cast<MAddress>(from));
    // Give the worker ordinary allocation space only after observing the
    // request. Both the table entry and completion are produced by relocate().
    destination->SetRegionAllocPtr(destination->GetRegionStart());
    if (generation.Workers() == nullptr) generation.InitializeWorkers(2);
    generation.Workers()->set_active_workers(2);
    generation.Workers()->set_active();
    generation.relocate().relocate(&generation.relocation_set());
    mutator.join();
    const MAddress mapping = forwarding->find(reinterpret_cast<MAddress>(from));
    const MAddress expected = destination->GetRegionStart();
    const bool valid = queued && blocked && before == 0 &&
        reinterpret_cast<MAddress>(result) == expected && mapping == expected && forwarding->is_done();
    std::fprintf(stderr,
        "RELOCATE_MISS_TARGET claimed=%d queued=%d blocked=%d before=%zx result=%zx mapping=%zx expected=%zx done=%d valid=%d\n",
        claimedPage, queued, blocked, before, reinterpret_cast<MAddress>(result), mapping,
        expected, forwarding->is_done(), valid);
    GC_EXPECT_TRUE(valid);
    for (ZPage* page : occupied) Heap::free_page(page);
    generation.reset_relocation_set();
}
}

GC_TEST(RelocateMiss782, ClaimedPageWaitsForWorkerPublication)
{
    ExerciseRelocationWait782(true);
}

GC_TEST(RelocateMiss782, AllocationFailureWaitsForWorkerPublication)
{
    ExerciseRelocationWait782(false);
}
