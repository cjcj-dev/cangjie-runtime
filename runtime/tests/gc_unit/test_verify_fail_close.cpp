// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_verify_fixture.hpp"
#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/ScopedObjectAccess.h"

#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_set>

#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zVerify.hpp"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "ObjectModel/MObject.h"
#include "ObjectModel/RefField.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

// Test-side signal sampling observes the product allocation top at rejection.
// It neither replaces a product call nor changes the collector's state.
ZPage* rejectedPage = nullptr;
volatile uintptr_t* rejectedTop = nullptr;
void RecordRejectedTop(int)
{
    *rejectedTop = rejectedPage->GetRegionAllocPtr();
    (void)signal(SIGABRT, SIG_DFL);
    (void)raise(SIGABRT);
}

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
        if (FILE* maps = std::fopen("/proc/self/maps", "r")) {
            char line[1024];
            std::fputs("VERIFY_PRODUCT_MAPS_BEGIN\n", stderr);
            while (std::fgets(line, sizeof(line), maps) != nullptr) { std::fputs(line, stderr); }
            std::fclose(maps);
            std::fputs("VERIFY_PRODUCT_MAPS_END\n", stderr);
        }
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

// zRelocate.cpp:1005-1008: verify actual forwarding output before releasing
// source liveness metadata. Enter through the product page relocation path.
GC_OTHER_VM_TEST(ZVerify, RelocationEntryRejectsBadLiveAccounting)
{
    if (!ZVerifyForwarding) {
        GC_EXPECT_EQ(setenv("ZVerifyForwarding", "1", 1), 0);
        RunInOtherVm("ZVerify.RelocationEntryRejectsBadLiveAccounting");
        return;
    }
    GcVerifyFixture fixture;
    fixture.PrepareOldSource();
    fixture.region0->SetRegionRole(ZPageRole::From);
    ExpectSceneAbort("Invalid number of live objects", [&] {
        fixture.region0->inc_live(1, RegionSpace::GetAllocSize(*fixture.obj0));
        RegionManager manager;
        manager.ForwardRegion<Generation::Old>(fixture.region0);
    });
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

// zVerify.cpp:610-634: the real relocation-page entry checks the inactive
// remembered face before copying any object. This is a phase-entry unit.
GC_OTHER_VM_TEST(ZVerify, RelocationEntryRejectsInactiveRemset)
{
    if (!ZVerifyRemembered) {
        GC_EXPECT_EQ(setenv("ZVerifyRemembered", "1", 1), 0);
        RunInOtherVm("ZVerify.RelocationEntryRejectsInactiveRemset");
        return;
    }
    GcVerifyFixture fixture;
    fixture.PrepareOldSource();
    auto* owner = forwarding_for_page(fixture.region0);
    GC_EXPECT_TRUE(owner != nullptr);
    fixture.region0->SetRegionRole(ZPageRole::From);
    RememberedSet& remset = HeapTestRemset();
    remset.Initialize(fixture.heapStart, 2 * ZPage::UNIT_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE;
    // ZGC zVerify.cpp:549-553: both remembered bits mean intentionally unremembered.
    HeapSlotAt<>(slot).StoreColoured(to_zpointer(raw(StoreGoodPointer(nullptr)) | ZPointerRememberedMask));
    // Empty is the positive control; then place one real field in the inactive face.
    ZVerify::BeforeRelocation(owner);
    remset.Record(slot);
    const bool currentActive = Heap::GetHeap().OldActiveRemsetIsCurrent();
    if (currentActive) { remset.FlipForMinor(); }
    const uintptr_t before = fixture.region0->GetRegionAllocPtr();
    void* shared = mmap(nullptr, sizeof(uintptr_t), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    GC_EXPECT_TRUE(shared != MAP_FAILED);
    auto* observed = static_cast<volatile uintptr_t*>(shared);
    *observed = 0;
    ExpectSceneAbort(currentActive ? "previous remset bits should be cleared" :
                                    "current remset bits should be cleared", [&] {
        rejectedPage = fixture.region0;
        rejectedTop = observed;
        (void)signal(SIGABRT, RecordRejectedTop);
        RegionManager manager;
        manager.ForwardRegion<Generation::Old>(fixture.region0);
    });
    const uintptr_t after = *observed;
    (void)munmap(shared, sizeof(uintptr_t));
    std::fprintf(stderr, "REMSET_REJECT_BEFORE_RESET_ASSERT_EXECUTED before=%#zx after=%#zx\n", before, after);
    // ZGC zRelocate.cpp:993-1008 verifies before do_forwarding mutates top.
    GC_EXPECT_EQ(after, before);
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

// ZGC's existing concurrent-GC breakpoint holds the real collector after
// following roots. Corrupt liveness there, then resume its actual mark-end.
GC_OTHER_VM_TEST(ZVerify, RuntimeRejectsLostLivenessAfterMark)
{
    if (!ZVerifyObjects) {
        GC_EXPECT_EQ(setenv("ZVerifyObjects", "1", 1), 0);
        RunInOtherVm("ZVerify.RuntimeRejectsLostLivenessAfterMark");
        return;
    }
    ExpectSceneAbort("Object verification failed", [&] {
        RuntimeParam param{};
        param.coParam.processorNum = 1;
        param.heapParam.heapSize = 32 * 1024;
        if (InitCJRuntime(&param) != E_OK) { _exit(121); }
        alignas(TypeInfo) unsigned char storage[sizeof(TypeInfo)]{};
        auto* type = reinterpret_cast<TypeInfo*>(storage);
        type->SetType(TypeKind::TYPE_KIND_CLASS);
        type->SetInstanceSize(sizeof(uintptr_t));
        auto* object = MObject::NewPinnedObject(type, 2 * sizeof(uintptr_t));
        if (object == nullptr) { _exit(122); }
        auto& heap = Heap::GetHeap();
        NativeSlot* root = heap.GetFinalizerProcessor().StrongRootStorage().Allocate();
        if (root == nullptr) { _exit(123); }
        root->StoreColoured(StoreGoodPointer(object));
        ConcurrentGCBreakpoints::AcquireControl();
        if (!ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED")) { _exit(124); }
        ZPage* page = Heap::page(reinterpret_cast<MAddress>(object));
        if (!page->is_object_live(from_object(object))) { _exit(125); }
        std::fprintf(stderr, "VERIFY_RUNTIME_MARK_END live_before=1 object=%p\n", object);
        page->reset_livemap();
        ConcurrentGCBreakpoints::RunToIdle();
        ConcurrentGCBreakpoints::ReleaseControl();
    });
}

// zMark.cpp:1022-1035: a fresh mark cycle cannot inherit published work.
// The breakpoint controller starts the real driver while retaining its pause.
GC_OTHER_VM_TEST(ZVerify, RuntimeRejectsStaleMarkStackAtStart)
{
    if (!ZVerifyMarking) {
        GC_EXPECT_EQ(setenv("ZVerifyMarking", "1", 1), 0);
        RunInOtherVm("ZVerify.RuntimeRejectsStaleMarkStackAtStart");
        return;
    }
    ExpectSceneAbort("Shared marking stripes are not empty", [&] {
        RuntimeParam param{};
        param.coParam.processorNum = 1;
        param.heapParam.heapSize = 32 * 1024;
        if (InitCJRuntime(&param) != E_OK) { _exit(121); }
        ConcurrentGCBreakpoints::AcquireControl();
        auto& stripes = Heap::GetHeap().old().Mark().Stripes();
        auto* stack = MarkStripeStack::Create(true);
        if (stack == nullptr) { _exit(122); }
        stack->Push(MarkStackEntry(uintptr_t(0x1000), true, true, true, false));
        stripes.At(0).PublishStack(stack, true);
        if (stripes.Population() != 1) { _exit(123); }
        std::fprintf(stderr, "VERIFY_RUNTIME_MARK_START published_stacks=1\n");
        // A broken start returns here before any worker consumes the deliberately
        // stale entry. Exit with the normal control result while GC is held.
        (void)ConcurrentGCBreakpoints::RunTo("AFTER MARKING STARTED");
        _exit(0);
    });
}


namespace {
enum class VerifyFieldCase {
    OldGood,
    OldUnmarked,
    WeakUnmarked,
    WeakPreviousRemembered,
    WeakMissingRemembered,
    WeakExactRemembered,
    WeakFinalizable,
    WeakYoungUnmarked,
    WeakYoungMarked,
    WeakNonLiveOld,
};

void RunVerifyFieldCycle(VerifyFieldCase mode)
{
    RuntimeParam param{};
    param.coParam.processorNum = 1;
    param.heapParam.heapSize = 32 * 1024;
    if (InitCJRuntime(&param) != E_OK) { _exit(121); }
    alignas(TypeInfo) unsigned char holderTypeStorage[sizeof(TypeInfo)]{};
    alignas(TypeInfo) unsigned char targetTypeStorage[sizeof(TypeInfo)]{};
    auto* holderType = reinterpret_cast<TypeInfo*>(holderTypeStorage);
    auto* targetType = reinterpret_cast<TypeInfo*>(targetTypeStorage);
    for (auto* type : {holderType, targetType}) {
        type->SetType(TypeKind::TYPE_KIND_CLASS);
        type->SetInstanceSize(sizeof(uintptr_t));
    }
    holderType->SetFlagHasRefField();
    GCTib tib{};
    tib.tag = SIGN_BIT | 1;
    holderType->SetGCTib(tib);
    auto* holder = MObject::NewPinnedObject(holderType, 2 * sizeof(uintptr_t));
    auto* target = MObject::NewPinnedObject(targetType, 2 * sizeof(uintptr_t));
    if (holder == nullptr || target == nullptr) { _exit(122); }
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreGoodPointer(target));
    auto& heap = Heap::GetHeap();
    NativeSlot* root = heap.GetFinalizerProcessor().StrongRootStorage().Allocate();
    if (root == nullptr) { _exit(123); }
    root->StoreColoured(StoreGoodPointer(holder));
    const bool afterWeak = mode != VerifyFieldCase::OldGood && mode != VerifyFieldCase::OldUnmarked;
    ConcurrentGCBreakpoints::AcquireControl();
    const char* point = afterWeak ? "AFTER CONCURRENT REFERENCE PROCESSING STARTED" :
                                   "BEFORE MARKING COMPLETED";
    if (!ConcurrentGCBreakpoints::RunTo(point)) { _exit(124); }
    ZPage* holderPage = Heap::page(reinterpret_cast<MAddress>(holder));
    ZPage* targetPage = Heap::page(reinterpret_cast<MAddress>(target));
    if (!holderPage->is_object_live(from_object(holder)) ||
        !targetPage->is_object_live(from_object(target))) { _exit(125); }
    uintptr_t value = raw(field.GetFieldValue());
    if (!ZPointer::is_marked_old(to_zpointer(value))) { _exit(126); }
    std::fprintf(stderr, "VERIFY_FIELD_QUALIFIED mode=%u holder=%p target=%p word=%#zx\n",
                 unsigned(mode), holder, target, value);
    switch (mode) {
        case VerifyFieldCase::OldGood:
            break;
        case VerifyFieldCase::OldUnmarked:
        case VerifyFieldCase::WeakUnmarked:
            value = (value ^ ZPointerMarkedOldMask) & ~ZPointerFinalizableMask;
            break;
        case VerifyFieldCase::WeakPreviousRemembered:
            value = (value & ~ZPointerRememberedMask) | (ZPointerRemembered ^ ZPointerRememberedMask);
            break;
        case VerifyFieldCase::WeakMissingRemembered:
            value = (value & ~ZPointerRememberedMask) | ZPointerRemembered;
            holderPage->clear_remset_bit_non_par_current(
                reinterpret_cast<MAddress>(&field) - holderPage->GetRegionStart());
            holderPage->clear_remset_previous();
            break;
        case VerifyFieldCase::WeakExactRemembered:
            value |= ZPointerRememberedMask;
            break;
        case VerifyFieldCase::WeakFinalizable:
            value = (value & ~ZPointerMarkedOldMask) | ZPointerFinalizable | ZPointerRememberedMask;
            break;
        case VerifyFieldCase::WeakYoungUnmarked:
        case VerifyFieldCase::WeakYoungMarked: {
            auto& manager = MutatorManager::Instance();
            if (manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD) == nullptr) { _exit(127); }
            {
                ScopedObjectAccess access;
                target = MObject::NewObject(targetType, 2 * sizeof(uintptr_t), AllocType::MOVEABLE_OBJECT);
                if (target == nullptr || !Heap::is_young(reinterpret_cast<MAddress>(target))) { _exit(128); }
                value = raw(StoreGoodPointer(target)) | ZPointerMarkedOld | ZPointerMarkedYoung |
                        ZPointerRememberedMask;
                if (mode == VerifyFieldCase::WeakYoungUnmarked) { value ^= ZPointerMarkedYoungMask; }
            }
            manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
            std::fprintf(stderr, "VERIFY_FIELD_YOUNG_TARGET target=%p word=%#zx\n", target, value);
            break;
        }
        case VerifyFieldCase::WeakNonLiveOld: {
            targetPage->reset_livemap();
            bool incLive = false;
            (void)holderPage->mark_object(from_object(holder), false, incLive);
            if (incLive) { holderPage->inc_live(1, holder->GetSize()); }
            if (!holderPage->is_object_live(from_object(holder)) ||
                targetPage->is_object_live(from_object(target))) { _exit(129); }
            value |= ZPointerRememberedMask;
            std::fprintf(stderr, "VERIFY_FIELD_LIVENESS holder_live=1 target_live=0\n");
            break;
        }
    }
    field.StoreColoured(to_zpointer(value));
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    BaseObject* result = ZBarrier::ReadStaticRef(*root);
    std::fprintf(stderr, "VERIFY_FIELD_CYCLE_COMPLETED mode=%u root=%p expected=%p\n",
                 unsigned(mode), result, holder);
    GC_EXPECT_TRUE(result == holder);
    heap.GetFinalizerProcessor().StrongRootStorage().Release(root);
}

void CheckVerifyFieldCase(VerifyFieldCase mode, const char* testName, const char* diagnostic)
{
    if (!ZVerifyObjects) {
        GC_EXPECT_EQ(setenv("ZVerifyObjects", "1", 1), 0);
        RunInOtherVm(testName);
        return;
    }
    if (diagnostic != nullptr) { ExpectSceneAbort(diagnostic, [=] { RunVerifyFieldCycle(mode); }); }
    else { RunVerifyFieldCycle(mode); }
}
}

GC_OTHER_VM_TEST(ZVerify, OldFieldAcceptsMarkedOldTarget)
{
    CheckVerifyFieldCase(VerifyFieldCase::OldGood, "ZVerify.OldFieldAcceptsMarkedOldTarget", nullptr);
}
GC_OTHER_VM_TEST(ZVerify, OldFieldRejectsUnmarkedOldTarget)
{
    CheckVerifyFieldCase(VerifyFieldCase::OldUnmarked, "ZVerify.OldFieldRejectsUnmarkedOldTarget", "Unmarked old oop");
}
GC_OTHER_VM_TEST(ZVerify, WeakFieldRejectsUnmarkedOldTarget)
{
    CheckVerifyFieldCase(VerifyFieldCase::WeakUnmarked, "ZVerify.WeakFieldRejectsUnmarkedOldTarget", "Bad possibly weak oop");
}
GC_OTHER_VM_TEST(ZVerify, WeakFieldRejectsPreviousRememberedColor)
{
    CheckVerifyFieldCase(VerifyFieldCase::WeakPreviousRemembered,
        "ZVerify.WeakFieldRejectsPreviousRememberedColor", "Previous remembered color");
}
GC_OTHER_VM_TEST(ZVerify, WeakFieldRejectsMissingRememberedBit)
{
    CheckVerifyFieldCase(VerifyFieldCase::WeakMissingRemembered,
        "ZVerify.WeakFieldRejectsMissingRememberedBit", "Missing remembered field");
}
GC_OTHER_VM_TEST(ZVerify, WeakFieldAcceptsExactRememberedColor)
{
    CheckVerifyFieldCase(VerifyFieldCase::WeakExactRemembered,
        "ZVerify.WeakFieldAcceptsExactRememberedColor", nullptr);
}
GC_OTHER_VM_TEST(ZVerify, WeakFieldAcceptsFinalizableColor)
{
    CheckVerifyFieldCase(VerifyFieldCase::WeakFinalizable,
        "ZVerify.WeakFieldAcceptsFinalizableColor", nullptr);
}

GC_OTHER_VM_TEST(ZVerify, WeakFieldRejectsUnmarkedYoungTarget)
{
    CheckVerifyFieldCase(VerifyFieldCase::WeakYoungUnmarked,
        "ZVerify.WeakFieldRejectsUnmarkedYoungTarget", "Unmarked young oop");
}
GC_OTHER_VM_TEST(ZVerify, WeakFieldAcceptsMarkedYoungTarget)
{
    CheckVerifyFieldCase(VerifyFieldCase::WeakYoungMarked,
        "ZVerify.WeakFieldAcceptsMarkedYoungTarget", nullptr);
}
GC_OTHER_VM_TEST(ZVerify, WeakFieldRejectsNonLiveOldTarget)
{
    CheckVerifyFieldCase(VerifyFieldCase::WeakNonLiveOld,
        "ZVerify.WeakFieldRejectsNonLiveOldTarget", "Non-live old oop");
}
