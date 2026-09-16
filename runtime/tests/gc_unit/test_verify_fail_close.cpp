// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_verify_fixture.hpp"
#include "gc_unittest.hpp"

#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>

#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zVerify.hpp"
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

} // namespace

// zVerify.cpp:119-128 / zAddress.inline.hpp:505-522: illegal addresses are
// rejected before metadata access; no region inventory or scene counters.
GC_OTHER_VM_TEST(ZVerify, RejectsColoredAddressWithoutUncoloring)
{
    GcVerifyFixture fixture;
    const uintptr_t colored = raw(ZAddress::store_good(from_object(fixture.obj0)));
    ExpectSceneAbort("Bad object", [&] {
        ZVerify::Object(reinterpret_cast<BaseObject*>(colored), &colored);
    });
    ZVerify::Object(fixture.obj0, &fixture.obj0);
}

GC_OTHER_VM_TEST(ZVerify, RejectsUnmanagedAddress)
{
    GcVerifyFixture fixture;
    ExpectSceneAbort("Bad object", [&] {
        ZVerify::Object(reinterpret_cast<BaseObject*>(0x1000), nullptr);
    });
    ZVerify::Object(fixture.obj0, &fixture.obj0);
}

GC_OTHER_VM_TEST(ZVerify, RememberedCurrentAndPreviousFaces)
{
    GcVerifyFixture fixture;
    RememberedSet& remset = Heap::GetHeap().GetRememberedSet();
    const MAddress slot = reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE;
    remset.Record(slot);
    GC_EXPECT_TRUE(remset.Contains(slot));
    GC_EXPECT_FALSE(remset.ContainsPrevious(slot));
    GC_EXPECT_FALSE(remset.IsClearInRange(fixture.heapStart, ZPage::UNIT_SIZE, true));
    remset.FlipForMinor();
    GC_EXPECT_FALSE(remset.Contains(slot));
    GC_EXPECT_TRUE(remset.ContainsPrevious(slot));
    GC_EXPECT_TRUE(remset.IsClearInRange(fixture.heapStart, ZPage::UNIT_SIZE, true));
}

// zForwarding.inline.hpp:116-119 / zVerify.cpp:601: installing a forwarding
// must leave the selected source object's start bit visible to iteration.
GC_OTHER_VM_TEST(ZVerify, SourcePreparationPreservesMarkedObjects)
{
    GcVerifyFixture fixture;
    fixture.PrepareOldSource();
    size_t visits = 0;
    fixture.region0->object_iterate([&](BaseObject* object) {
        GC_EXPECT_TRUE(object == fixture.obj0);
        ++visits;
    });
    GC_EXPECT_EQ(visits, size_t(1));
    std::fprintf(stderr, "SOURCE_MARKED_OBJECT_ASSERT_EXECUTED visits=%zu\n", visits);
}

// test_zForwarding.cpp:ZForwardingTest.find_full plus zForwarding.cpp:369-409.
GC_OTHER_VM_TEST(ZVerify, ForwardingTableChecksLiveAccounting)
{
    GcVerifyFixture fixture;
    fixture.PrepareOldSource();
    auto publication = ForwardingTable::EnsurePublicationBeforeCopy(
        fixture.region0, reinterpret_cast<MAddress>(fixture.obj0));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication,
        reinterpret_cast<MAddress>(fixture.obj0), reinterpret_cast<MAddress>(fixture.obj1)),
        reinterpret_cast<MAddress>(fixture.obj1));
    auto owner = forwarding_for_page(fixture.region0);
    GC_EXPECT_TRUE(static_cast<bool>(owner));
    owner->verify();
    ExpectSceneAbort("Invalid number of live objects", [&] {
        fixture.region0->inc_live(1, RegionSpace::GetAllocSize(*fixture.obj0));
        owner->verify();
    });
    owner->verify();
    ExpectSceneAbort("Invalid number of live bytes", [&] {
        fixture.region0->inc_live(0, RegionSpace::GetAllocSize(*fixture.obj0));
        owner->verify();
    });
    owner->verify();
}

// zVerify.cpp:531-609: the source field must be represented in the active
// remembered face. This exercises the actual verifier, not just bitmap reads.
GC_OTHER_VM_TEST(ZVerify, BeforeRelocationRejectsMissingRememberedField)
{
    if (!ZVerifyRemembered) {
        GC_EXPECT_EQ(setenv("ZVerifyRemembered", "1", 1), 0);
        RunInOtherVm("ZVerify.BeforeRelocationRejectsMissingRememberedField");
        return;
    }
    GcVerifyFixture fixture;
    fixture.PrepareOldSource();
    auto publication = ForwardingTable::EnsurePublicationBeforeCopy(
        fixture.region0, reinterpret_cast<MAddress>(fixture.obj0));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    auto owner = forwarding_for_page(fixture.region0);
    GC_EXPECT_TRUE(static_cast<bool>(owner));
    const MAddress slot = reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE;
    HeapSlotAt<>(slot).StoreColoured(StoreGoodPointer(fixture.obj1));
    RememberedSet& remset = Heap::GetHeap().GetRememberedSet();
    remset.Initialize(fixture.heapStart, 2 * ZPage::UNIT_SIZE);
    ExpectSceneAbort("Missing remembered field", [&] { ZVerify::BeforeRelocation(owner.get()); });
    remset.Record(slot);
    if (!Heap::GetHeap().GetCollector().OldActiveRemsetIsCurrent()) { remset.FlipForMinor(); }
    ZVerify::BeforeRelocation(owner.get());
}

// zVerify.cpp:131-138 distinguishes raw null from metadata-bearing null.
GC_OTHER_VM_TEST(ZVerify, RawNullRequiresYoungMarkComplete)
{
    GcVerifyFixture fixture;
    auto& cycle = LiveMapCycleAccess::Cycle(Heap::GetHeap().GetCollector(), Generation::Young);
    cycle.PublishPhase(GC_PHASE_TRACE);
    RefField<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(zpointer::null);
    ExpectSceneAbort("Raw null requires young mark complete", [&] {
        ZVerify::Oop(fixture.obj0, field, false);
    });
    // Weak-inclusive nulls have no raw-null restriction in zVerify.
    ZVerify::Oop(fixture.obj0, field, true);
    field.StoreColoured(to_zpointer(::g_cjStoreGoodMask));
    ZVerify::Oop(fixture.obj0, field, false);
}

GC_OTHER_VM_TEST(ZVerify, RawNullRequiresAllocatingHolder)
{
    GcVerifyFixture fixture;
    auto& cycle = LiveMapCycleAccess::Cycle(Heap::GetHeap().GetCollector(), Generation::Young);
    cycle.PublishPhase(GC_PHASE_MARK_COMPLETE);
    RefField<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(zpointer::null);
    // A page from a previous owner cycle is relocatable.
    ExpectSceneAbort("Raw null requires allocating holder", [&] {
        ZVerify::Oop(fixture.obj0, field, false);
    });
    // Reset establishes a new allocating page, independent of object offsets.
    fixture.region0->ResetPageSequence();
    const MAddress next = fixture.region0->GetRegionAllocPtr();
    BaseObject* fresh = fixture.PlaceObject(next);
    fixture.region0->SetRegionAllocPtr(next + 64);
    RefField<>& freshField = HeapSlotAt<>(next + TYPEINFO_PTR_SIZE);
    freshField.StoreColoured(zpointer::null);
    ZVerify::Oop(fresh, freshField, false);
}
