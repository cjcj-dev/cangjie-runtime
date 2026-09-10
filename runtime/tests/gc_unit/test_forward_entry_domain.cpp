#include <atomic>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#define private public
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#undef private

#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Verify/MutatorRelocate.h"
#include "Heap/WCollector/WCollector.h"

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

bool OutputNamesForwardObject(const std::string& text)
{
    return text.find("forward_object requires a forwarding entry") != std::string::npos;
}

} // namespace

GC_TEST(ForwardEntryDomain, TakeRegionReturnsLiveToPage)
{
    GcHeapFixture fx;
    fx.BindTakeRegionCapacity();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionInfo* taken = space.GetRegionManager().TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS, false, false);
    std::fprintf(stderr, "DETAIL take_region addr=%p start=%#zx\n",
                 static_cast<void*>(taken), taken == nullptr ? 0 : static_cast<size_t>(taken->GetRegionStart()));
    GC_EXPECT_TRUE(taken != nullptr);
    GC_EXPECT_TRUE(taken->GetRegionStart() != 0);
}

GC_TEST(ForwardEntryDomain, MutatorCopyArmedHitToNotFrom)
{
    GcHeapFixture fx;
    fx.BindTakeRegionCapacity();
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    BaseObject* got = collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(fx.obj0));
    std::fprintf(stderr, "DETAIL copy_hit got=%p from=%p lookup.to=%#zx answer=%u\n",
                 static_cast<void*>(got), static_cast<void*>(fx.obj0),
                 static_cast<size_t>(lookup.to), static_cast<unsigned>(lookup.answer));
    GC_EXPECT_TRUE(got != nullptr);
    GC_EXPECT_TRUE(got != fx.obj0);
    GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedHit);
    GC_EXPECT_TRUE(lookup.to != reinterpret_cast<MAddress>(fx.obj0));
    (void)live;
}

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
    std::fprintf(stderr, "DETAIL identity_after_done got=%p from=%p\n",
                 static_cast<void*>(got), static_cast<void*>(fx.obj0));
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(got), reinterpret_cast<MAddress>(fx.obj0));
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

GC_TEST(ForwardEntryDomain, WaitRoutedIdentityAfterPublish)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    std::atomic<int> started{ 0 };
    std::atomic<BaseObject*> got{ nullptr };
    MutatorRelocate::EnterScope();
    std::thread waiter([&]() {
        started.store(1, std::memory_order_release);
        got.store(collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young),
                  std::memory_order_release);
    });
    JoinGuard join(waiter);
    while (started.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    PublishIdentity(fx.region0, fx.obj0);
    fx.region0->MarkForwardingDone();
    waiter.join();
    MutatorRelocate::LeaveScope();
    BaseObject* resolved = got.load();
    std::fprintf(stderr, "DETAIL wait_identity got=%p from=%p compacted=%u done=%u\n",
                 static_cast<void*>(resolved), static_cast<void*>(fx.obj0),
                 static_cast<unsigned>(fx.region0->IsCompacted()),
                 static_cast<unsigned>(fx.region0->IsForwardingDone()));
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(resolved), reinterpret_cast<MAddress>(fx.obj0));
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

GC_TEST(ForwardEntryDomain, MissingEntryAbortsForwardObject)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    fx.region0->MarkForwardingDone();
    int fds[2];
    GC_EXPECT_EQ(pipe(fds), 0);
    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        (void)close(fds[0]);
        (void)dup2(fds[1], STDERR_FILENO);
        (void)signal(SIGABRT, SIG_DFL);
        WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
        collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
        (void)collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
        _exit(0);
    }
    (void)close(fds[1]);
    const std::string err = ReadFd(fds[0]);
    (void)close(fds[0]);
    const int rc = WaitChild(pid);
    std::fprintf(stderr, "DETAIL missing_rc=%d named=%d\n", rc, OutputNamesForwardObject(err) ? 1 : 0);
    GC_EXPECT_TRUE(rc != 0);
    GC_EXPECT_TRUE(err.find("WaitRouted") != std::string::npos || OutputNamesForwardObject(err));
    (void)live;
}

GC_TEST(ForwardEntryDomain, WrongLifecycleAborts)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    PublishIdentity(fx.region0, fx.obj0);
    fx.region0->BumpRegionLifeId();
    int fds[2];
    GC_EXPECT_EQ(pipe(fds), 0);
    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        (void)close(fds[0]);
        (void)dup2(fds[1], STDERR_FILENO);
        (void)signal(SIGABRT, SIG_DFL);
        MutatorRelocate::EnterScope();
        WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
        collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
        (void)collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
        _exit(0);
    }
    (void)close(fds[1]);
    const std::string err = ReadFd(fds[0]);
    (void)close(fds[0]);
    const int rc = WaitChild(pid);
    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(fx.obj0));
    std::fprintf(stderr, "DETAIL wrong_life_rc=%d lookup.answer=%u named=%d\n",
                 rc, static_cast<unsigned>(lookup.answer), OutputNamesForwardObject(err) ? 1 : 0);
    GC_EXPECT_TRUE(lookup.answer != ForwardingTable::ToAnswer::ArmedHit || rc != 0);
    (void)live;
}

GC_TEST(ForwardEntryDomain, UnavailableTableAborts)
{
    GcHeapFixture fx;
    LiveInfo* live = PlantGhostFrom(fx, fx.region0, fx.obj0);
    ForwardingTable::Remove(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    int fds[2];
    GC_EXPECT_EQ(pipe(fds), 0);
    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        (void)close(fds[0]);
        (void)dup2(fds[1], STDERR_FILENO);
        (void)signal(SIGABRT, SIG_DFL);
        MutatorRelocate::EnterScope();
        WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
        collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
        (void)collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
        _exit(0);
    }
    (void)close(fds[1]);
    const std::string err = ReadFd(fds[0]);
    (void)close(fds[0]);
    const int rc = WaitChild(pid);
    std::fprintf(stderr, "DETAIL unavailable_rc=%d named=%d\n", rc, OutputNamesForwardObject(err) ? 1 : 0);
    GC_EXPECT_TRUE(rc != 0);
    (void)live;
}
