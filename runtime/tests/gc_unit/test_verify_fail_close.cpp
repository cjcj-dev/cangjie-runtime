// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>

#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Verify/VerifyHeap.h"
#include "Heap/Verify/VerifyRegions.h"
#include "Heap/Verify/VerifyRememberedSet.h"
#include "ObjectModel/RefField.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

template <typename Fn>
void ExpectSceneAbort(const char* expectedDiagnostic, Fn&& fn)
{
    int childStderr[2];
    GC_EXPECT_EQ(pipe(childStderr), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(childStderr[0]);
        if (dup2(childStderr[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(childStderr[1]);
        (void)signal(SIGABRT, SIG_DFL);
        fn();
        _exit(0);
    }
    close(childStderr[1]);
    std::string transcript;
    char buffer[512];
    for (;;) {
        const ssize_t count = read(childStderr[0], buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        transcript.append(buffer, static_cast<size_t>(count));
    }
    close(childStderr[0]);
    (void)std::fwrite(transcript.data(), 1, transcript.size(), stderr);
    std::fflush(stderr);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    GC_EXPECT_TRUE(transcript.find(expectedDiagnostic) != std::string::npos);
}

RegionManager& InstallFixtureWalk(GcHeapFixture& fixture)
{
    auto& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    manager.regionHeapStart = fixture.heapStart;
    manager.inactiveZone = fixture.heapStart + GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE;

    fixture.obj0 = fixture.PlaceObject(fixture.region0->GetRegionStart());
    fixture.obj1 = fixture.PlaceObject(fixture.region1->GetRegionStart());
    fixture.region0->SetRegionAllocPtr(fixture.region0->GetRegionStart() + fixture.obj0->GetSize());
    fixture.region1->SetRegionAllocPtr(fixture.region1->GetRegionStart() + fixture.obj1->GetSize());
    return manager;
}

HeapSlot<>& ObjectField(BaseObject* object)
{
    return HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
}

GC_OTHER_VM_TEST(VerifyFailClose, HeapBadTargetReachesSceneAssertion)
{
    ExpectSceneAbort("[GCV2][verify][heap] scene failed", [] {
        (void)setenv("MRT_GCV2_VERIFY_HEAP", "1", 1);
        GcHeapFixture fixture;
        (void)InstallFixtureWalk(fixture);
        BaseObject* invalidTarget = reinterpret_cast<BaseObject*>(
            fixture.heapStart + 2 * RegionInfo::UNIT_SIZE + 64);
        ObjectField(fixture.obj0).StoreColoured(StoreGoodPointer(invalidTarget));
        VerifyHeapObjects("gc-unit-bad-target");
    });
}

GC_OTHER_VM_TEST(VerifyFailClose, MissingRemsetReachesSceneAssertion)
{
    ExpectSceneAbort("[GCV2][verify][remset] scene failed", [] {
        (void)setenv("MRT_GCV2_DIAG", "remembered", 1);
        GcHeapFixture fixture;
        (void)InstallFixtureWalk(fixture);
        fixture.region0->SetRegionType(RegionInfo::RegionType::RECENT_FULL_REGION);
        fixture.region1->SetYoungRegionFlag(1);
        ObjectField(fixture.obj0).StoreColoured(StoreGoodPointer(fixture.obj1));
        const std::unordered_set<MAddress> emptyRemset;
        VerifyRememberedSetInvariant("gc-unit-missing-remset", emptyRemset);
    });
}

GC_OTHER_VM_TEST(VerifyFailClose, MissingRegionCandidateReachesSceneAssertion)
{
    ExpectSceneAbort("[GCV2][verify][regions] scene failed", [] {
        (void)setenv("MRT_GCV2_VERIFY_REGIONS", "1", 1);
        GcHeapFixture fixture;
        RegionManager& manager = InstallFixtureWalk(fixture);
        fixture.region0->SetYoungRegionFlag(1);
        manager.fromRegionList.PrependRegion(fixture.region0, RegionInfo::RegionType::FROM_REGION);
        const VerifyRegions::CandidateSet emptyCandidates;
        VerifyRegions::VerifyAfterPrepareYoung(
            manager, emptyCandidates, 1, "gc-unit-missing-candidate");
    });
}

} // namespace
