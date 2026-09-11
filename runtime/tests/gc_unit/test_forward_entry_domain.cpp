#include <csignal>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#define private public
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Allocator/FreeRegionManager.h"
#undef private

#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Verify/MutatorRelocate.h"
#include "Heap/WCollector/WCollector.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" size_t MRT_PublishKeptInPlaceReceiptsForTest(RegionInfo* region);

namespace {

int WaitChild(pid_t pid)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

std::string ReadFd(int fd)
{
    std::string out;
    char buf[512];
    ssize_t n = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

void SeedFreeUnits(GcHeapFixture& fx)
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    auto& manager = space.GetRegionManager();
    manager.regionHeapStart = fx.heapStart;
    manager.regionHeapEnd = fx.heapStart + GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE;
    manager.inactiveZone.store(fx.heapStart + 2 * RegionInfo::UNIT_SIZE);
    manager.maxUnitCountPerRegion = 1;
    manager.freeRegionManager.Initialize(GcHeapFixture::kUnits);
    for (size_t index = 2; index < GcHeapFixture::kUnits; ++index) {
        (void)RegionInfo::InitRegion(index, 1, RegionInfo::UnitRole::FREE_UNITS);
        GC_EXPECT_TRUE(manager.freeRegionManager.dirtyUnitTree.MergeInsert(index, 1, false));
    }
}

LiveInfo* PlantGhostFrom(GcHeapFixture& fx, RegionInfo* region, BaseObject* object)
{
    region->SetYoungRegionFlag(1);
    region->SetYoungAge(1);
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize());
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(object));
    (void)region->MarkObject(region->GetMarkView<Generation::Young>(), object, 8);
    region->AddLiveByteCount(8);
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Young>());
    region->RecordRouteStart(offset);
    return live;
}

void PublishMoved(RegionInfo* region, BaseObject* fromObj, BaseObject* toObj)
{
    const MAddress from = reinterpret_cast<MAddress>(fromObj);
    const MAddress to = reinterpret_cast<MAddress>(toObj);
    ForwardingTable::Publication publication =
        ForwardingTable::RetainOpenPublicationAfterCopy(region, from);
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    const ZForwarding::Receipt receipt = ForwardingTable::InstallMapping(publication, from, to);
    GC_EXPECT_EQ(receipt.address, to);
}

bool NamesForwardObject(const std::string& text)
{
    return text.find("forward_object requires a forwarding entry") != std::string::npos;
}

bool NamesWaitHole(const std::string& text)
{
    return text.find("WaitRoutedTipReady") != std::string::npos ||
           text.find("published-without-receipt") != std::string::npos;
}

} // namespace

GC_TEST(ForwardEntryDomain, WaitRoutedIdentityFromKeptProducer)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    GC_EXPECT_FALSE(fx.region0->IsForwardingDone());
    GC_EXPECT_FALSE(fx.region0->IsCompacted());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    ForwardingTable::SetLookupRetainHook(
        [](void* raw) {
            auto* region = static_cast<RegionInfo*>(raw);
            const size_t published = MRT_PublishKeptInPlaceReceiptsForTest(region);
            std::fprintf(stderr, "DETAIL kept_producer receipts=%zu\n", published);
        },
        fx.region0);
    RegionInfo::DrainScope drain(fx.region0, MutatorRelocate::Retire::DISPEL_GHOST);
    BaseObject* before = collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
    ForwardingTable::SetLookupRetainHook(nullptr, nullptr);
    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(fx.obj0));
    std::fprintf(stderr, "DETAIL wait_identity got=%p from=%p answer=%u to=%#zx done=%u compacted=%u\n",
                 static_cast<void*>(before), static_cast<void*>(fx.obj0),
                 static_cast<unsigned>(lookup.answer), static_cast<size_t>(lookup.to),
                 static_cast<unsigned>(fx.region0->IsForwardingDone()),
                 static_cast<unsigned>(fx.region0->IsCompacted()));
    GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedHit);
    GC_EXPECT_EQ(lookup.to, reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(before), reinterpret_cast<MAddress>(fx.obj0));
    fx.region0->MarkForwardingDone();
    BaseObject* after = collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
    std::fprintf(stderr, "DETAIL wait_identity_after_done got=%p\n", static_cast<void*>(after));
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(after), reinterpret_cast<MAddress>(fx.obj0));
    (void)live;
}

GC_TEST(ForwardEntryDomain, NonIdentityArmedHitReturnsToBeforeAndAfterDone)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    PublishMoved(fx.region0, fx.obj0, fx.obj1);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    BaseObject* got = collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
    std::fprintf(stderr, "DETAIL non_identity_before_done got=%p to=%p\n",
                 static_cast<void*>(got), static_cast<void*>(fx.obj1));
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(got), reinterpret_cast<MAddress>(fx.obj1));
    fx.region0->MarkForwardingDone();
    got = collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
    std::fprintf(stderr, "DETAIL non_identity_after_done got=%p to=%p\n",
                 static_cast<void*>(got), static_cast<void*>(fx.obj1));
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(got), reinterpret_cast<MAddress>(fx.obj1));
    (void)live;
}

GC_TEST(ForwardEntryDomain, RetiredHitWithoutGhostReturnsTo)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    PublishMoved(fx.region0, fx.obj0, fx.obj1);
    fx.region0->MarkForwardingDone();
    ForwardingTable::ClearEntries(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    fx.region0->DispelGhostFromRegion();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    BaseObject* got = collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
    std::fprintf(stderr, "DETAIL retired got=%p to=%p\n",
                 static_cast<void*>(got), static_cast<void*>(fx.obj1));
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(got), reinterpret_cast<MAddress>(fx.obj1));
    (void)live;
}

static void RunNamedAbort(const char* label, BaseObject* object)
{
    int fds[2];
    GC_EXPECT_EQ(pipe(fds), 0);
    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        (void)close(fds[0]);
        (void)dup2(fds[1], STDERR_FILENO);
        (void)signal(SIGABRT, SIG_DFL);
        RegionInfo* region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(object));
        RegionInfo::DrainScope drain(region, MutatorRelocate::Retire::DISPEL_GHOST);
        WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
        collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
        (void)collector.relocate_or_remap_object(object, ZGenerationId::young);
        _exit(0);
    }
    (void)close(fds[1]);
    const std::string err = ReadFd(fds[0]);
    (void)close(fds[0]);
    const int rc = WaitChild(pid);
    const bool named = NamesForwardObject(err) || NamesWaitHole(err);
    std::fprintf(stderr, "DETAIL %s rc=%d named=%d\n", label, rc, named ? 1 : 0);
    if (!named) {
        std::fwrite(err.data(), 1, err.size(), stderr);
    }
    GC_EXPECT_TRUE(rc != 0);
    GC_EXPECT_TRUE(named);
}

GC_TEST(ForwardEntryDomain, MissingEntryAbortsForwardObject)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    fx.region0->MarkForwardingDone();
    RunNamedAbort("missing", fx.obj0);
    (void)live;
}

GC_TEST(ForwardEntryDomain, UnavailableTableAborts)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    ForwardingTable::Remove(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    RunNamedAbort("unavailable", fx.obj0);
    (void)live;
}
