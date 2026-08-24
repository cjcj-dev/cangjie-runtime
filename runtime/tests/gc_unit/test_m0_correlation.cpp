// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#if defined(MRT_M0_CORRELATION_EXPERIMENT) && defined(MRT_GC_UNIT_TEST_ACCESS)

#include <cstdio>
#include <cstring>
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "Cangjie.h"
#include "gc_heap_fixture.hpp"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionManager.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Verify/M0Correlation.h"
#include "Heap/Verify/M0ExitDiagnostics.h"
#include "Heap/WCollector/WCollector.h"
#include "TypeInfoManager.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo* klass, MSize size);
extern "C" ObjRef MCC_NewPinnedObject(const TypeInfo* klass, MSize size, bool isFinalizer);

// This friend already exists for the product relocation receipt suite.  These
// methods only arrange the product entry preconditions; copying, receipt
// installation, correlation propagation and compacting remain in the SO.
struct RelocationReceiptTestAccess {
    static void BindCollector(CollectorResources& resources, TracingCollector* collector)
    {
        resources.collectorProxy.currentCollector = collector;
    }

    static void ParkFrom(RegionManager& manager, RegionInfo* region)
    {
        manager.fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
    }

    static BaseObject* ForwardExclusive(WCollector& collector, BaseObject* from,
                                        BaseObject* to, RegionInfo* copyPage)
    {
        return collector.ForwardObjectExclusive(from, to, copyPage);
    }
};
} // namespace MapleRuntime

namespace {
M0Correlation::ObjectStamp ValidStamp(uintptr_t address, uintptr_t start, uint64_t life)
{
    M0Correlation::ObjectStamp stamp;
    stamp.valid = true;
    stamp.address = address;
    stamp.regionStart = start;
    stamp.regionLife = life;
    stamp.offset = address - start;
    return stamp;
}

bool IsClass(const char* value, const char* expected)
{
    return std::strcmp(value, expected) == 0;
}

TypeInfo* ProductClassInfo()
{
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)];
    static bool initialized = false;
    if (!initialized) {
        std::memset(storage, 0, sizeof(storage));
        TypeInfo* info = reinterpret_cast<TypeInfo*>(storage);
        info->SetType(TypeKind::TYPE_KIND_CLASS);
        info->SetInstanceSize(sizeof(void*));
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(storage), sizeof(storage));
        initialized = true;
    }
    return reinterpret_cast<TypeInfo*>(storage);
}

MSize ProductObjectSize()
{
    return static_cast<MSize>(AlignUp<size_t>(ProductClassInfo()->GetInstanceSize(), 8) + TYPEINFO_PTR_SIZE);
}

int RunRuntimeCase(CJTaskFunc task, uintptr_t argument)
{
#if defined(__linux__)
    const pid_t child = fork();
    if (child == 0) {
        (void)setenv("cjProcessorNum", "1", 1);
        RuntimeParam param {};
        param.heapParam.heapSize = 32 * 1024;
        param.coParam.processorNum = 1;
        if (InitCJRuntime(&param) != E_OK) {
            _exit(100);
        }
        CJThreadHandle handle = RunCJTask(task, reinterpret_cast<void*>(argument));
        if (handle == nullptr) {
            _exit(101);
        }
        void* result = nullptr;
        if (GetTaskRet(handle, &result) != E_OK) {
            _exit(102);
        }
        ReleaseHandle(handle);
        const uintptr_t status = reinterpret_cast<uintptr_t>(result);
        if (FiniCJRuntime() != E_OK) {
            _exit(103);
        }
        _exit(status > 99 ? 99 : static_cast<int>(status));
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return 120;
    }
    return WEXITSTATUS(status);
#else
    (void)task;
    (void)argument;
    return 0;
#endif
}

void* RunMoveableBindObserveRelease(void*)
{
    constexpr uint64_t key = 0x401;
    M0Correlation::ResetForTest();
    M0Correlation::SelectKeepLive(0);
    M0Correlation::TagNextAllocation(key);
    BaseObject* object = reinterpret_cast<BaseObject*>(MCC_NewObject(ProductClassInfo(), ProductObjectSize()));
    size_t failures = object == nullptr ? 1 : 0;
    const M0Correlation::AllocationToken token = M0Correlation::ExternalTokenForTest(key);
    failures += token == 0 ? 1 : 0;
    if (object != nullptr) {
        const M0Correlation::ObjectStamp stamp = M0Correlation::CaptureStamp(object);
        failures += stamp.valid ? 0 : 1;
        failures += M0Correlation::LookupStampForTest(stamp) == token ? 0 : 1;
        M0Correlation::Observe(object, key, 77, 81, 93);
    }
    M0Correlation::TestSnapshot beforeRelease = M0Correlation::SnapshotForTest();
    failures += beforeRelease.binds == 1 ? 0 : 1;
    failures += beforeRelease.observations == 1 ? 0 : 1;
    failures += beforeRelease.contractErrors == 0 ? 0 : 1;
    M0Correlation::Release(key);
    M0Correlation::TestSnapshot afterRelease = M0Correlation::SnapshotForTest();
    failures += afterRelease.releases == 1 ? 0 : 1;
    failures += M0Correlation::FooterValidForTest() ? 0 : 1;
    std::fprintf(stderr, "[M0CORR_PRODUCT_BIND] failures=%zu token=%llu\n", failures,
                 static_cast<unsigned long long>(token));
    return reinterpret_cast<void*>(failures);
}

void* RunPinnedReject(void*)
{
    constexpr uint64_t key = 0x402;
    M0Correlation::ResetForTest();
    M0Correlation::TagNextAllocation(key);
    BaseObject* pinned = reinterpret_cast<BaseObject*>(
        MCC_NewPinnedObject(ProductClassInfo(), ProductObjectSize(), false));
    BaseObject* laterMoveable = reinterpret_cast<BaseObject*>(
        MCC_NewObject(ProductClassInfo(), ProductObjectSize()));
    const M0Correlation::TestSnapshot snapshot = M0Correlation::SnapshotForTest();
    size_t failures = pinned == nullptr || laterMoveable == nullptr ? 1 : 0;
    failures += snapshot.tagRejected == 1 ? 0 : 1;
    failures += snapshot.binds == 0 ? 0 : 1;
    failures += snapshot.contractErrors == 1 ? 0 : 1;
    failures += M0Correlation::ExternalTokenForTest(key) == 0 ? 0 : 1;
    failures += !M0Correlation::FooterValidForTest() ? 0 : 1;
    std::fprintf(stderr, "[M0CORR_PINNED_REJECT] failures=%zu rejected=%llu binds=%llu\n", failures,
                 static_cast<unsigned long long>(snapshot.tagRejected),
                 static_cast<unsigned long long>(snapshot.binds));
    return reinterpret_cast<void*>(failures);
}

void* RunKeepLive(void* rawTreatment)
{
    const bool treatment = reinterpret_cast<uintptr_t>(rawTreatment) != 0;
    const uint64_t key = treatment ? 0x404 : 0x403;
    M0Correlation::ResetForTest();
    M0Correlation::SelectKeepLive(treatment ? key : 0);
    M0Correlation::TagNextAllocation(key);
    BaseObject* object = reinterpret_cast<BaseObject*>(MCC_NewObject(ProductClassInfo(), ProductObjectSize()));
    size_t hitsBefore = 0;
    Heap::GetHeap().VisitAllExportRoots([&](ObjectRef& root) {
        if (raw(root.LoadPlain()) == reinterpret_cast<uintptr_t>(object)) {
            ++hitsBefore;
        }
    });
    M0Correlation::TestSnapshot before = M0Correlation::SnapshotForTest();
    M0Correlation::Release(key);
    size_t hitsAfter = 0;
    Heap::GetHeap().VisitAllExportRoots([&](ObjectRef& root) {
        if (raw(root.LoadPlain()) == reinterpret_cast<uintptr_t>(object)) {
            ++hitsAfter;
        }
    });
    M0Correlation::TestSnapshot after = M0Correlation::SnapshotForTest();
    size_t failures = object == nullptr ? 1 : 0;
    failures += treatment ? (hitsBefore == 1 ? 0 : 1) : (hitsBefore == 0 ? 0 : 1);
    failures += hitsAfter == 0 ? 0 : 1;
    failures += before.interventionRegister == (treatment ? 1u : 0u) ? 0 : 1;
    failures += after.interventionUnregister == (treatment ? 1u : 0u) ? 0 : 1;
    failures += M0Correlation::FooterValidForTest() ? 0 : 1;
    std::fprintf(stderr, "[M0CORR_KEEP_LIVE] treatment=%u failures=%zu before=%zu after=%zu\n",
                 treatment ? 1u : 0u, failures, hitsBefore, hitsAfter);
    return reinterpret_cast<void*>(failures);
}

void* RunPinnedSlotReuse(void*)
{
    constexpr uint64_t key = 0x405;
    M0Correlation::ResetForTest();
    BaseObject* pinned = reinterpret_cast<BaseObject*>(
        MCC_NewPinnedObject(ProductClassInfo(), ProductObjectSize(), false));
    size_t failures = pinned == nullptr ? 1 : 0;
    if (pinned == nullptr) {
        return reinterpret_cast<void*>(failures);
    }
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(pinned));
    const M0Correlation::ObjectStamp oldStamp = M0Correlation::CaptureStamp(pinned);
    const M0Correlation::AllocationToken oldToken = M0Correlation::BindStampForTest(key, oldStamp);
    RegionManager& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    const size_t reclaimed = manager.CollectFreePinnedSlots(region);
    const uintptr_t reused = manager.AllocPinnedFromFreeList(ProductObjectSize());
    const M0Correlation::TestSnapshot snapshot = M0Correlation::SnapshotForTest();
    failures += oldToken == 0 ? 1 : 0;
    failures += reclaimed == ProductObjectSize() ? 0 : 1;
    failures += reused == reinterpret_cast<uintptr_t>(pinned) ? 0 : 1;
    failures += M0Correlation::LookupStampForTest(oldStamp) == 0 ? 0 : 1;
    failures += snapshot.forwards == 0 ? 0 : 1;
    M0Correlation::Release(key);
    failures += M0Correlation::FooterValidForTest() ? 0 : 1;
    std::fprintf(stderr, "[M0CORR_PINNED_REUSE] failures=%zu reclaimed=%zu reused=%#lx\n",
                 failures, reclaimed, static_cast<unsigned long>(reused));
    return reinterpret_cast<void*>(failures);
}

LiveInfo* PrepareForwardable(GcHeapFixture& fx, RegionInfo* region, MAddress liveObject)
{
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    BaseObject* object = reinterpret_cast<BaseObject*>(liveObject);
    (void)bitmap->MarkBits(region->GetAddressOffset(liveObject), object->GetSize(), region->GetRegionSize());
    region->AddLiveByteCount(object->GetSize());
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
    return live;
}

void CleanupForwardable(GcHeapFixture& fx, RegionInfo* region, LiveInfo* live)
{
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::DropRetiredCovering(region->GetRegionStart(), region->GetRegionSize());
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}
} // namespace

GC_TEST(M0Correlation, NewAllocationAtSameStampGetsNewToken)
{
    M0Correlation::ResetForTest();
    const M0Correlation::ObjectStamp stamp = ValidStamp(0x100028u, 0x100000u, 7);
    const M0Correlation::AllocationToken first = M0Correlation::BindStampForTest(11, stamp);
    const M0Correlation::AllocationToken second = M0Correlation::BindStampForTest(12, stamp);
    GC_EXPECT_NE(first, 0u);
    GC_EXPECT_NE(second, 0u);
    GC_EXPECT_NE(first, second);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(stamp), second);
}

GC_TEST(M0Correlation, InvalidPresentEndpointInvalidatesCandidate)
{
    const M0Correlation::ObjectStamp good = ValidStamp(0x200030u, 0x200000u, 9);
    GC_EXPECT_TRUE(IsClass(M0Correlation::ClassifyEvidenceForTest(
        true, good, true, good, false, {}, false, {}), "VALID"));

    M0Correlation::ObjectStamp zeroLife = good;
    zeroLife.regionLife = 0;
    M0Correlation::ObjectStamp inconsistent = good;
    inconsistent.address += 8;

    const auto invalid = [&](bool targetPresent, const M0Correlation::ObjectStamp& target,
                             bool consumerPresent, const M0Correlation::ObjectStamp& consumer,
                             bool activePresent, const M0Correlation::ObjectStamp& active,
                             bool retiredPresent, const M0Correlation::ObjectStamp& retired) {
        GC_EXPECT_TRUE(IsClass(M0Correlation::ClassifyEvidenceForTest(
            targetPresent, target, consumerPresent, consumer, activePresent, active,
            retiredPresent, retired), "INVALID_EVIDENCE"));
    };

    invalid(true, zeroLife, true, good, false, {}, false, {});
    invalid(true, inconsistent, true, good, false, {}, false, {});
    invalid(true, good, true, zeroLife, false, {}, false, {});
    invalid(true, good, true, inconsistent, false, {}, false, {});
    invalid(true, good, true, good, true, zeroLife, false, {});
    invalid(true, good, true, good, true, inconsistent, false, {});
    invalid(true, good, true, good, false, {}, true, zeroLife);
    invalid(true, good, true, good, false, {}, true, inconsistent);

    // S0 carries no active/retired endpoint; absence is evidence, not an
    // invented invalid stamp.
    GC_EXPECT_TRUE(IsClass(M0Correlation::ClassifyEvidenceForTest(
        true, good, true, good, false, zeroLife, false, inconsistent), "VALID"));
}

GC_TEST(M0Correlation, RegionResetReuseMintsNewToken)
{
    GC_EXPECT_TRUE(M0Correlation::Enabled());
    GcHeapFixture fx;
    M0Correlation::ResetForTest();
    const M0Correlation::ObjectStamp oldStamp = M0Correlation::CaptureStamp(fx.obj0);
    const M0Correlation::AllocationToken oldToken = M0Correlation::BindStampForTest(21, oldStamp);
    GC_EXPECT_NE(oldToken, 0u);

    fx.region0->InitRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    fx.region0->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    const M0Correlation::ObjectStamp newStamp = M0Correlation::CaptureStamp(fx.obj0);
    GC_EXPECT_NE(newStamp.regionLife, oldStamp.regionLife);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(oldStamp), 0u);

    const M0Correlation::AllocationToken newToken = M0Correlation::BindStampForTest(22, newStamp);
    GC_EXPECT_NE(newToken, 0u);
    GC_EXPECT_NE(newToken, oldToken);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(newStamp), newToken);
}

GC_TEST(M0Correlation, LedgerBypassesHumanDetailCap)
{
    GC_EXPECT_TRUE(M0Correlation::Enabled());
    GcHeapFixture fx;
    M0Correlation::ResetForTest();
    const M0Correlation::ObjectStamp stamp = M0Correlation::CaptureStamp(fx.obj0);
    const M0Correlation::AllocationToken token = M0Correlation::BindStampForTest(31, stamp);
    GC_EXPECT_NE(token, 0u);

    const M0ExitDiagnostics::Counts before = M0ExitDiagnostics::GetCounts();
    constexpr uint64_t kEvents = 35;
    for (uint64_t i = 0; i < kEvents; ++i) {
        M0ExitDiagnostics::Note(M0ExitDiagnostics::Exit::ReadBarrier, fx.obj0, nullptr, fx.obj1, 7);
    }
    const M0ExitDiagnostics::Counts after = M0ExitDiagnostics::GetCounts();
    const M0Correlation::TestSnapshot ledger = M0Correlation::SnapshotForTest();
    GC_EXPECT_EQ(after.total, before.total + kEvents);
    GC_EXPECT_EQ(after.sampled, M0ExitDiagnostics::kDetailedSampleLimit);
    GC_EXPECT_EQ(ledger.m0Seen, kEvents);
    GC_EXPECT_EQ(ledger.m0Written, kEvents);
    M0Correlation::Release(31);
}

GC_TEST(M0Correlation, ProductAllocationBindsAndObservesManagedReference)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunMoveableBindObserveRelease, 0), 0);
}

GC_TEST(M0Correlation, TagBeforePinnedAllocationFailsClosed)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunPinnedReject, 0), 0);
}

GC_TEST(M0Correlation, KeepLiveControlDoesNotRegisterExportRoot)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunKeepLive, 0), 0);
}

GC_TEST(M0Correlation, KeepLiveTreatmentRegistersAndReleasesExportRoot)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunKeepLive, 1), 0);
}

GC_TEST(M0Correlation, OrdinaryRelocationPropagatesProductToken)
{
    GcHeapFixture fx;
    M0Correlation::ResetForTest();
    (void)ForwardingTable::Initialize(
        fx.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);
    const MAddress from = reinterpret_cast<MAddress>(fx.obj0);
    const MAddress to = reinterpret_cast<MAddress>(fx.obj1);
    const M0Correlation::ObjectStamp fromStamp = M0Correlation::CaptureStamp(from);
    const M0Correlation::AllocationToken token = M0Correlation::BindStampForTest(0x501, fromStamp);

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, fx.region0, from);
    RegionSpace& productSpace = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RelocationRequestQueue& requests = productSpace.GetRegionManager().GetRelocationRequestQueue();
    requests.BeginWorkers(1);
    const auto requested = requests.Add(fx.region0, from);
    GC_EXPECT_TRUE(requested.accepted);
    StateWord oldWord = fx.obj0->GetStateWord();
    GC_EXPECT_TRUE(fx.obj0->TryLockObject(oldWord));
    fx.region0->NoteCopyInflight();

    BaseObject* relocated = RelocationReceiptTestAccess::ForwardExclusive(
        collector, fx.obj0, fx.obj1, fx.region0);
    const bool published = requested.request->state() == RelocationRequestQueue::State::COMPLETED;
    if (!published) {
        (void)requests.Fail(from);
    }
    GC_EXPECT_TRUE(published);
    GC_EXPECT_TRUE(relocated == fx.obj1);
    GC_EXPECT_EQ(requests.Wait(requested.request), to);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(fromStamp), token);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(M0Correlation::CaptureStamp(to)), token);
    GC_EXPECT_EQ(M0Correlation::SnapshotForTest().forwards, 1u);
    GC_EXPECT_TRUE(requests.SynchronizePoll().workersDone);

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    CleanupForwardable(fx, fx.region0, live);
}

GC_TEST(M0Correlation, FullCompactSameLifeKeepsAllocationToken)
{
    GcHeapFixture fx;
    M0Correlation::ResetForTest();
    (void)ForwardingTable::Initialize(
        fx.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);
    RegionInfo* region = fx.region0;
    const MAddress start = region->GetRegionStart();
    BaseObject* overwritten = fx.PlaceObject(start);
    const size_t objectSize = overwritten->GetSize();
    BaseObject* survivor = fx.PlaceObject(start + objectSize);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(survivor) + objectSize);
    const M0Correlation::ObjectStamp overwrittenStamp = M0Correlation::CaptureStamp(overwritten);
    const M0Correlation::ObjectStamp survivorStamp = M0Correlation::CaptureStamp(survivor);
    const M0Correlation::AllocationToken overwrittenToken =
        M0Correlation::BindStampForTest(0x601, overwrittenStamp);
    const M0Correlation::AllocationToken survivorToken =
        M0Correlation::BindStampForTest(0x602, survivorStamp);

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(survivor));
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    manager.CompactRegion(region);

    const M0Correlation::ObjectStamp destination = M0Correlation::CaptureStamp(start);
    GC_EXPECT_EQ(destination.regionLife, survivorStamp.regionLife);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(destination), survivorToken);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(survivorStamp), survivorToken);
    GC_EXPECT_NE(overwrittenToken, survivorToken);
    GC_EXPECT_EQ(M0Correlation::SnapshotForTest().forwards, 1u);
    const M0Correlation::AllocationToken newToken = M0Correlation::BindStampForTest(0x603, destination);
    GC_EXPECT_NE(newToken, survivorToken);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(destination), newToken);

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    CleanupForwardable(fx, region, live);
}

GC_TEST(M0Correlation, PartialCompactSameLifeKeepsAllocationToken)
{
    GcHeapFixture fx;
    M0Correlation::ResetForTest();
    (void)ForwardingTable::Initialize(
        fx.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);
    RegionInfo* region = fx.region0;
    RegionInfo* firstDestination = fx.region1;
    const MAddress start = region->GetRegionStart();
    BaseObject* overwritten = fx.PlaceObject(start);
    const size_t objectSize = overwritten->GetSize();
    BaseObject* survivor = fx.PlaceObject(start + objectSize);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(survivor) + objectSize);
    firstDestination->SetRegionAllocPtr(firstDestination->GetRegionEnd());
    const M0Correlation::ObjectStamp overwrittenStamp = M0Correlation::CaptureStamp(overwritten);
    const M0Correlation::ObjectStamp survivorStamp = M0Correlation::CaptureStamp(survivor);
    const M0Correlation::AllocationToken overwrittenToken =
        M0Correlation::BindStampForTest(0x701, overwrittenStamp);
    const M0Correlation::AllocationToken survivorToken =
        M0Correlation::BindStampForTest(0x702, survivorStamp);

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(survivor));
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    manager.CompactRegion(region, firstDestination);

    const M0Correlation::ObjectStamp destination = M0Correlation::CaptureStamp(start);
    GC_EXPECT_EQ(destination.regionLife, survivorStamp.regionLife);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(destination), survivorToken);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(survivorStamp), survivorToken);
    GC_EXPECT_NE(overwrittenToken, survivorToken);
    GC_EXPECT_EQ(M0Correlation::SnapshotForTest().forwards, 1u);
    const M0Correlation::AllocationToken newToken = M0Correlation::BindStampForTest(0x703, destination);
    GC_EXPECT_NE(newToken, survivorToken);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(destination), newToken);

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    CleanupForwardable(fx, region, live);
}

GC_TEST(M0Correlation, PinnedFreeListReuseClearsOldBinding)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunPinnedSlotReuse, 0), 0);
}

GC_TEST(M0Correlation, MissingM0RecordInvalidatesFooter)
{
    GcHeapFixture fx;
    M0Correlation::ResetForTest();
    const M0Correlation::ObjectStamp stamp = M0Correlation::CaptureStamp(fx.obj0);
    GC_EXPECT_NE(M0Correlation::BindStampForTest(0x801, stamp), 0u);
    M0Correlation::DropNextM0WriteForTest();
    M0ExitDiagnostics::Note(M0ExitDiagnostics::Exit::ReadBarrier, fx.obj0, nullptr, nullptr, 3);
    const M0Correlation::TestSnapshot snapshot = M0Correlation::SnapshotForTest();
    GC_EXPECT_EQ(snapshot.m0Seen, 1u);
    GC_EXPECT_EQ(snapshot.m0Written, 0u);
    GC_EXPECT_FALSE(M0Correlation::FooterValidForTest());
}

GC_TEST(M0Correlation, PaintedHeaderWithoutReceiptIsInvalidEvidence)
{
    GcHeapFixture fx;
    M0Correlation::ResetForTest();
    const M0Correlation::ObjectStamp stamp = M0Correlation::CaptureStamp(fx.obj0);
    GC_EXPECT_NE(M0Correlation::BindStampForTest(0x802, stamp), 0u);
    fx.obj0->SetStateCode(ObjectState::FORWARDED);
    M0ExitDiagnostics::Note(M0ExitDiagnostics::Exit::ReadBarrier, fx.obj0, nullptr, nullptr, 4);
    const M0Correlation::TestSnapshot snapshot = M0Correlation::SnapshotForTest();
    GC_EXPECT_EQ(snapshot.m0Seen, 1u);
    GC_EXPECT_EQ(snapshot.m0Written, 1u);
    GC_EXPECT_EQ(snapshot.contractErrors, 1u);
    GC_EXPECT_FALSE(M0Correlation::FooterValidForTest());
}

#endif
