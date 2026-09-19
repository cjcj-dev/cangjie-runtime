// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_verify_fixture.hpp"
#include "gc_unittest.hpp"
#include "Cangjie.h"

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
    const bool target = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
        transcript.find(expectedDiagnostic) != std::string::npos;
    std::fprintf(stderr, "VERIFY_TARGET_ASSERT_EXECUTED diagnostic=%s status=%d matched=%d\n",
                 expectedDiagnostic, status, target);
    GC_EXPECT_TRUE(target);
}

} // namespace

// Verification enters through a product phase, not a public test-only leaf.
GC_OTHER_VM_TEST(ZVerify, BeforeOperationRejectsUnmanagedRoot)
{
    if (!ZVerifyRoots) {
        GC_EXPECT_EQ(setenv("ZVerifyRoots", "1", 1), 0);
        RunInOtherVm("ZVerify.BeforeOperationRejectsUnmanagedRoot");
        return;
    }
    GcVerifyFixture fixture;
    ExpectSceneAbort("Bad object", [&] {
        fixture.VerifyRoot(reinterpret_cast<BaseObject*>(fixture.heapStart + 3 * ZPage::UNIT_SIZE));
    });
    fixture.VerifyRoot(fixture.obj0);
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
    auto publication = forwarding_for_page(
        fixture.region0, reinterpret_cast<MAddress>(fixture.obj0));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(UNUSED_InsertMapping(publication,
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
    auto publication = forwarding_for_page(
        fixture.region0, reinterpret_cast<MAddress>(fixture.obj0));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    auto owner = forwarding_for_page(fixture.region0);
    GC_EXPECT_TRUE(static_cast<bool>(owner));
    const MAddress slot = reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE;
    HeapSlotAt<>(slot).StoreColoured(StoreGoodPointer(fixture.obj1));
    RememberedSet& remset = HeapTestRemset();
    remset.Initialize(fixture.heapStart, 2 * ZPage::UNIT_SIZE);
    ExpectSceneAbort("Missing remembered field", [&] { ZVerify::BeforeRelocation(owner); });
    remset.Record(slot);
    if (!Heap::GetHeap().OldActiveRemsetIsCurrent()) { remset.FlipForMinor(); }
    ZVerify::BeforeRelocation(owner);
}

// zVerify.cpp:131-138 distinguishes raw null from metadata-bearing null.
GC_OTHER_VM_TEST(ZVerify, RawNullRequiresYoungMarkComplete)
{
    if (!ZVerifyObjects) {
        GC_EXPECT_EQ(setenv("ZVerifyObjects", "1", 1), 0);
        RunInOtherVm("ZVerify.RawNullRequiresYoungMarkComplete");
        return;
    }
    GcVerifyFixture fixture;
    fixture.region0->reset(PageAge::old);
    auto& cycle = Heap::GetHeap().GetZGeneration(Generation::Young);
    cycle.PublishPhase(ZGenerationPhase::Mark);
    RefField<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(zpointer::null);
    ExpectSceneAbort("Raw null requires young mark complete", [&] {
        fixture.VerifyObject(fixture.obj0, false);
    });
    // Weak-inclusive nulls have no raw-null restriction in zVerify.
    fixture.VerifyObject(fixture.obj0, true);
    field.StoreColoured(to_zpointer(::g_cjStoreGoodMask));
    fixture.VerifyObject(fixture.obj0, false);
}

GC_OTHER_VM_TEST(ZVerify, RawNullRequiresAllocatingHolder)
{
    if (!ZVerifyObjects) {
        GC_EXPECT_EQ(setenv("ZVerifyObjects", "1", 1), 0);
        RunInOtherVm("ZVerify.RawNullRequiresAllocatingHolder");
        return;
    }
    GcVerifyFixture fixture;
    fixture.region0->reset(PageAge::old);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    auto& cycle = Heap::GetHeap().GetZGeneration(Generation::Young);
    cycle.PublishPhase(ZGenerationPhase::MarkComplete);
    RefField<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(zpointer::null);
    // A page from a previous owner cycle is relocatable.
    ExpectSceneAbort("Raw null requires allocating holder", [&] {
        fixture.VerifyObject(fixture.obj0, false);
    });
    // Reset establishes a new allocating page, independent of object offsets.
    fixture.region0->ResetPageSequence();
    const MAddress next = fixture.region0->GetRegionAllocPtr();
    BaseObject* fresh = fixture.PlaceObject(next);
    fixture.region0->SetRegionAllocPtr(next + 64);
    RefField<>& freshField = HeapSlotAt<>(next + TYPEINFO_PTR_SIZE);
    freshField.StoreColoured(zpointer::null);
    fixture.VerifyObject(fresh, false);
}

// Enter through the real collector request and VM operation. The invalid root
// is installed before collection; the test never calls the verifier itself.
GC_OTHER_VM_TEST(ZVerify, RuntimeRejectsUnallocatedRootBeforeMark)
{
    if (!ZVerifyRoots) {
        GC_EXPECT_EQ(setenv("ZVerifyRoots", "1", 1), 0);
        RunInOtherVm("ZVerify.RuntimeRejectsUnallocatedRootBeforeMark");
        return;
    }
    ExpectSceneAbort("Bad object", [&] {
        RuntimeParam param{};
        param.coParam.processorNum = 1;
        param.heapParam.heapSize = 32 * 1024;
        if (InitCJRuntime(&param) != E_OK) { _exit(121); }
        auto& heap = Heap::GetHeap();
        const MAddress bad = heap.GetAllocator().GetSpaceEndAddress() - sizeof(void*);
        // Existence qualification precedes the target check and has a distinct rc.
        if (Heap::is_in(bad)) { _exit(122); }
        NativeSlot* root = heap.GetFinalizerProcessor().StrongRootStorage().Allocate();
        if (root == nullptr) { _exit(123); }
        root->StoreColoured(ZAddress::store_good(static_cast<zaddress>(bad)));
        std::fprintf(stderr, "VERIFY_RUNTIME_REQUEST root=%p address=%#zx\n", root, bad);
        heap.RequestGC(GC_REASON_USER, false);
    });
}
