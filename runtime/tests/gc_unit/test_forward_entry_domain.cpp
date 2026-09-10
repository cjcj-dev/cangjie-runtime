#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#define private public
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#undef private

#include "Heap/WCollector/WCollector.h"
#include "Heap/Allocator/ForwardingTable.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

int WaitChild(pid_t pid)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFSIGNALED(status)) {
        return WTERMSIG(status);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

void EnterIsolatedChild()
{
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        (void)dup2(devnull, STDERR_FILENO);
        (void)dup2(devnull, STDOUT_FILENO);
        (void)close(devnull);
    }
    (void)signal(SIGABRT, SIG_DFL);
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

void PublishIdentity(RegionInfo* region, BaseObject* object)
{
    const MAddress from = reinterpret_cast<MAddress>(object);
    ForwardingTable::Publication publication =
        ForwardingTable::RetainOpenPublicationAfterCopy(region, from);
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    const ZForwarding::Receipt receipt = ForwardingTable::InstallMapping(publication, from, from);
    GC_EXPECT_EQ(receipt.address, from);
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

} // namespace

GC_TEST(ForwardEntryDomain, IdentityArmedHitBeforeDoneReturnsFrom)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    PublishIdentity(fx.region0, fx.obj0);
    GC_EXPECT_FALSE(fx.region0->IsForwardingDone());
    GC_EXPECT_FALSE(fx.region0->IsCompacted());
    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedHit);
    GC_EXPECT_EQ(lookup.to, reinterpret_cast<MAddress>(fx.obj0));
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    BaseObject* got = collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
    std::fprintf(stderr, "DETAIL identity_before_done got=%p from=%p\n",
                 static_cast<void*>(got), static_cast<void*>(fx.obj0));
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(got), reinterpret_cast<MAddress>(fx.obj0));
    fx.region0->MarkForwardingDone();
    got = collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(got), reinterpret_cast<MAddress>(fx.obj0));
    (void)live;
}

GC_TEST(ForwardEntryDomain, NonIdentityArmedHitReturnsTo)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    PublishMoved(fx.region0, fx.obj0, fx.obj1);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    BaseObject* got = collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
    std::fprintf(stderr, "DETAIL non_identity got=%p to=%p\n",
                 static_cast<void*>(got), static_cast<void*>(fx.obj1));
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(got), reinterpret_cast<MAddress>(fx.obj1));
    (void)live;
}

GC_TEST(ForwardEntryDomain, RetiredHitWithoutGhostReturnsTo)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    PublishMoved(fx.region0, fx.obj0, fx.obj1);
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

GC_TEST(ForwardEntryDomain, MissingEntryAborts)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    fx.region0->MarkForwardingDone();
    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        EnterIsolatedChild();
        WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
        collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
        (void)collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
        _exit(0);
    }
    GC_EXPECT_EQ(WaitChild(pid), SIGABRT);
    (void)live;
}

GC_TEST(ForwardEntryDomain, UnavailableTableAborts)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    ForwardingTable::Remove(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        EnterIsolatedChild();
        WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
        collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
        (void)collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
        _exit(0);
    }
    GC_EXPECT_EQ(WaitChild(pid), SIGABRT);
    (void)live;
}

GC_TEST(ForwardEntryDomain, NotReadyWithoutPublisherAborts)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        EnterIsolatedChild();
        WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
        collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
        (void)collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
        _exit(0);
    }
    GC_EXPECT_EQ(WaitChild(pid), SIGABRT);
    (void)live;
}
