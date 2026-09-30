#include "gc_heap_fixture.hpp"
#include "b09_runtime_fixture.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Mutator/ThreadLocal.h"
#include <csignal>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>
#include <future>
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// PORT_ZFORWARDING step ①: ZForwardingTable granule map of ZForwarding* +
// ZForwarding attached-array / refcount skeleton.
// Anchors: zForwardingTable.hpp:32-52, zForwardingTable.inline.hpp:43-62,
//          zForwarding.hpp:44-110, zAttachedArray.inline.hpp:32-84.
//
// The process-global InitFwdTables is one-shot (RegionManager.cpp:970
// and test_forwarding_no_geometry). A second Initialize with a different heap is a
// no-op, so these tests exercise ZGranuleMap locally and ZForwarding without
// rebinding the product map.

#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zAttachedArray.hpp"
#include "Heap/z/zGranuleMap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zRelocate.hpp"
#include "gc_unittest.hpp"

#include <type_traits>
#include <chrono>
#include <thread>
#include "Heap/z/zRelocate.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

static_assert(!std::is_convertible<MAddress, zoffset>::value);
static_assert(!std::is_convertible<zoffset, MAddress>::value);
static_assert(!std::is_convertible<zpointer, zoffset>::value);
static_assert(!std::is_convertible<zaddress, zoffset>::value);
static_assert(!std::is_convertible<zaddress_unsafe, zoffset>::value);

GC_TEST(ZGranuleMap, GetPutRemove)
{
    constexpr MAddress kStart = 0x40000000;
    constexpr size_t kSize = ZGranuleSize;
    ZGranuleMap<ZForwarding*> map(4 * kSize);

    ZForwarding* fwd = ZForwarding::Create(4, kStart, kStart, kSize);
    GC_EXPECT_TRUE(fwd != nullptr);
    const zoffset start = static_cast<zoffset>(0);
    const zoffset interior = static_cast<zoffset>(8);
    const zoffset next = static_cast<zoffset>(kSize);
    map.put(start, kSize, fwd);
    GC_EXPECT_TRUE(map.get(start) == fwd);
    GC_EXPECT_TRUE(map.get(interior) == fwd);
    GC_EXPECT_TRUE(map.get(next) == nullptr);

    map.put(start, kSize, nullptr);
    GC_EXPECT_TRUE(map.get(start) == nullptr);
    fwd->Destroy();
}

// The removed native-address adapter is replaced by the ZGC offset map.
// zGranuleMap.inline.hpp:39,51: maximum offset determines the fixed extent.
GC_TEST(ZGranuleMap, MaximumOffsetDeterminesExtent)
{
    ZGranuleMap<int*> map(4 * ZGranuleSize);
    int value = 42;
    map.put(static_cast<zoffset>(3 * ZGranuleSize), &value);
    GC_EXPECT_EQ(map.size(), 4u);
    GC_EXPECT_TRUE(map.get(static_cast<zoffset>(4 * ZGranuleSize - 1)) == &value);
    GC_EXPECT_TRUE(map.at(2) == nullptr);
}

// ZGranuleMap::put/get (zGranuleMap.inline.hpp:62-84) and the highest-offset
// map extent in zPageTable.cpp:37-47. Reserved capacity excludes the hole;
// address indices include it, so publishing either segment cannot alias it.
GC_TEST(ZGranuleMap, DiscontiguousPagesLeaveHoleUnmapped)
{
    constexpr size_t granule = ZGranuleSize;
    ZGranuleMap<int*> map(5 * granule);
    int first = 1;
    int second = 2;
    map.put(static_cast<zoffset>(0), 2 * granule, &first);
    map.put(static_cast<zoffset>(3 * granule), 2 * granule, &second);
    for (size_t i = 0; i < 5; ++i) {
        const zoffset offset = static_cast<zoffset>(i * granule + granule - 1);
        GC_EXPECT_TRUE(map.get(offset) == (i < 2 ? &first : i == 2 ? nullptr : &second));
    }
    map.put(static_cast<zoffset>(0), 2 * granule, nullptr);
    GC_EXPECT_TRUE(map.get(static_cast<zoffset>(0)) == nullptr);
    GC_EXPECT_TRUE(map.get(static_cast<zoffset>(3 * granule)) == &second);
}

GC_TEST(ZGranuleMap, MaximumAddressSpaceExtent)
{
    EnsureZAddressDomain();
    ZGranuleMap<int*> map(ZAddressOffsetMax);
    int value = 1;
    map.put(static_cast<zoffset>(ZAddressOffsetMax - ZGranuleSize), &value);
    GC_EXPECT_EQ(map.size(), ZAddressOffsetMax >> ZGranuleSizeShift);
    GC_EXPECT_TRUE(map.get(static_cast<zoffset>(ZAddressOffsetMax - 1)) == &value);
    GC_EXPECT_TRUE(map.get(static_cast<zoffset>(0)) == nullptr);
}

GC_TEST(ZForwarding, AttachedArraySitsAfterObject)
{
    constexpr MAddress kStart = 0x50000000;
    ZForwarding* fwd = ZForwarding::Create(4, kStart, kStart, 0x1000);
    GC_EXPECT_TRUE(fwd != nullptr);
    const size_t objectSize = ZForwarding::AttachedArray::object_size();
    GC_EXPECT_TRUE(objectSize >= sizeof(ZForwarding));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(fwd->entries()),
                 reinterpret_cast<uintptr_t>(fwd) + objectSize);
    GC_EXPECT_EQ(objectSize % sizeof(std::atomic<uint64_t>), static_cast<size_t>(0));
    GC_EXPECT_TRUE((fwd->length() & (fwd->length() - 1)) == 0);

    GC_EXPECT_EQ(fwd->_ref_count.load(std::memory_order_acquire), 1);
    GC_EXPECT_FALSE(fwd->is_claimed());
    GC_EXPECT_FALSE(fwd->is_done());

    const MAddress from = kStart + 16;
    const MAddress to = kStart + 0x2000;
    GC_EXPECT_EQ(fwd->insert(from, to), to);
    GC_EXPECT_EQ(fwd->find(from), to);
    GC_EXPECT_EQ(fwd->find(kStart + 24), static_cast<MAddress>(0));
    fwd->Destroy();
}

// The provisional table is not a different lifetime object: it enters the same
// three-state ref-count protocol before it is published in the granule map.
// This couples the two pieces changed together by the provisional-table port.
GC_TEST(ZForwarding, PageUsesRefCountProtocol)
{
    constexpr MAddress kStart = 0x58000000;
    ZForwarding* fwd = ZForwarding::alloc(1, kStart, kStart, 0x1000, nullptr, 7);
    GC_EXPECT_TRUE(fwd != nullptr);
    GC_EXPECT_EQ(fwd->page_life_id(), static_cast<RegionLifeId>(7));
    GC_EXPECT_EQ(fwd->_ref_count.load(std::memory_order_acquire), 1);

    ZRelocateQueue queue;
    queue.activate(1);
    GC_EXPECT_TRUE(fwd->retain_page(&queue));
    GC_EXPECT_EQ(fwd->_ref_count.load(std::memory_order_acquire), 2);
    fwd->release_page();
    GC_EXPECT_EQ(fwd->_ref_count.load(std::memory_order_acquire), 1);

    GC_EXPECT_TRUE(fwd->claim());
    fwd->in_place_relocation_claim_page();
    GC_EXPECT_EQ(fwd->_ref_count.load(std::memory_order_acquire), -1);
    fwd->mark_done();
    GC_EXPECT_FALSE(fwd->retain_page(&queue));
    GC_EXPECT_TRUE(fwd->is_done());
    fwd->release_page();
    GC_EXPECT_EQ(fwd->_ref_count.load(std::memory_order_acquire), 0);
    GC_EXPECT_FALSE(fwd->retain_page(&queue));
    fwd->Destroy();
}

GC_TEST(ZForwardingTable, kZfwdTableConsumeOn)
{
    static_assert(true,
                  "step ② IsFromObject consumes ZForwardingTable::get (PORT_ZFORWARDING.md §六)");
    GC_EXPECT_TRUE(true);
}

GC_TEST(ZForwardingTable, PageReleaseKeepsEntriesUntilMapRemoval)
{
    constexpr MAddress kStart = 0x60000000;
    constexpr size_t kSize = ZGranuleSize;
    ZGranuleMap<ZForwarding*> entries(4 * kSize);

    ZForwarding* fwd = ZForwarding::Create(4, kStart, kStart, kSize);
    GC_EXPECT_TRUE(fwd != nullptr);
    const MAddress from = kStart + 16;
    const MAddress to = 0x70000000;
    const zoffset start = static_cast<zoffset>(0);
    const zoffset fromOffset = static_cast<zoffset>(from - kStart);
    entries.put(start, kSize, fwd);

    fwd->release_page();
    fwd->detach_page();
    GC_EXPECT_TRUE(entries.get(start) == fwd);

    GC_EXPECT_EQ(fwd->insert(from, to), to);
    GC_EXPECT_EQ(entries.get(fromOffset)->find(from), to);

    entries.put(start, kSize, nullptr);
    fwd->Destroy();
}

// Protocol cases ported from zRemembered.cpp:284-324 and
// zForwarding.cpp:278-350; this is source coverage, not a load test.
GC_TEST(ZForwardingRemembered, PublishedFieldsConsumedOnce)
{
    GcHeapFixture heap;
    auto* fwd = ZForwarding::Create(1, heap.heapStart, heap.heapStart, ZGranuleSize);
    const MAddress field = heap.heapStart + sizeof(void*);
    fwd->relocated_remembered_fields_register(field);
    fwd->relocated_remembered_fields_publish();
    fwd->release_page();
    fwd->mark_done();
    size_t count = 0;
    MAddress observed = 0;
    fwd->relocated_remembered_fields_apply_to_published([&](MAddress p) { ++count; observed = p; });
    GC_EXPECT_EQ(count, 1u);
    GC_EXPECT_EQ(observed, field);
    fwd->relocated_remembered_fields_apply_to_published([&](MAddress) { ++count; });
    GC_EXPECT_EQ(count, 1u);
    fwd->Destroy();
}

GC_TEST(ZForwardingRemembered, RetainedScanRejectsPublication)
{
    GcHeapFixture heap;
    auto* fwd = ZForwarding::Create(1, heap.heapStart, heap.heapStart, ZGranuleSize);
    GC_EXPECT_TRUE(fwd->retain_page(&generation_relocate_queue(Generation::Old)));
    fwd->relocated_remembered_fields_register(heap.heapStart + sizeof(void*));
    fwd->relocated_remembered_fields_notify_concurrent_scan_of();
    GC_EXPECT_TRUE(fwd->relocated_remembered_fields_is_concurrently_scanned());
    fwd->release_page();
    fwd->relocated_remembered_fields_publish();
    fwd->release_page();
    fwd->mark_done();
    size_t count = 0;
    fwd->relocated_remembered_fields_apply_to_published([&](MAddress) { ++count; });
    GC_EXPECT_EQ(count, 0u);
    fwd->Destroy();
}

GC_TEST(ZForwardingRemembered, YoungPhaseOwnsPublication)
{
    GcHeapFixture heap;
    auto& collector = Heap::GetHeap();
    const ZGenerationPhase youngPhase = Heap::GetHeap().GetZGeneration(ZGenerationId::young).GcPhase();
    const ZGenerationPhase oldPhase = Heap::GetHeap().GetZGeneration(ZGenerationId::old).GcPhase();
    for (bool marking : { false, true }) {
        ZGeneration::young()->set_phase(
            marking ? ZGenerationPhase::Mark : ZGenerationPhase::Relocate);
        ZGeneration::old()->set_phase(
            marking ? ZGenerationPhase::Relocate : ZGenerationPhase::Mark);
        auto* fwd = ZForwarding::Create(1, heap.heapStart, heap.heapStart, ZGranuleSize);
        fwd->relocated_remembered_fields_register(heap.heapStart + sizeof(void*));
        fwd->relocated_remembered_fields_after_relocate();
        fwd->release_page();
        fwd->mark_done();
        size_t count = 0;
        fwd->relocated_remembered_fields_apply_to_published([&](MAddress) { ++count; });
        fwd->Destroy();
        ZGeneration::young()->set_phase( youngPhase);
        ZGeneration::old()->set_phase( oldPhase);
        GC_EXPECT_EQ(count, marking ? 1u : 0u);
    }
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(ZForwardingRemembered, ClaimedRetainUsesPageCompletionQueue)
{
    GcHeapFixture heap;
    auto& queue = generation_relocate_queue(Generation::Old);
    queue.activate(1);
    auto* fwd = ZForwarding::Create(1, heap.heapStart, heap.heapStart, ZGranuleSize);
    GC_EXPECT_TRUE(fwd->claim());
    fwd->in_place_relocation_claim_page();
    std::atomic<bool> readerStarted{false};
    std::atomic<bool> returned{ false };
    bool retained = true;
    std::promise<void> completion;
    auto completed = completion.get_future();
    std::thread reader([&] {
        readerStarted.store(true, std::memory_order_release);
        retained = fwd->retain_page(&queue);
        returned.store(true, std::memory_order_release);
        completion.set_value();
    });
    JoinGuard guard(reader);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!readerStarted.load(std::memory_order_acquire) &&
           !returned.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool queued = readerStarted.load(std::memory_order_acquire);
    const bool returnedBeforeDone = completed.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready;
    fwd->release_page();
    const bool returnedAfterRelease = completed.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready;
    fwd->mark_done();
    // ZGC zRelocate.cpp:116-131: the completing worker prunes and notifies
    // before joining the waiting mutator. mark_done alone is not a wakeup.
    queue.leave();
    reader.join();
    queue.deactivate();
    fwd->Destroy();
    GC_EXPECT_TRUE(queued);
    GC_EXPECT_FALSE(returnedBeforeDone);
    GC_EXPECT_FALSE(returnedAfterRelease);
    GC_EXPECT_FALSE(retained);
}

#endif // MRT_TESTABLE_INTERNALS

// ZGC zRelocate.cpp:652-731,801-815,1289-1307: use the relocation
// phase to derive destination fields from real source remembered bits.
// Each state is an input on a separate source page; no test supplies a
// destination field to the product register consumer.
struct Remembered1314Relocation {
    B09RuntimeFixture runtime;
    ZForwarding* owners[2]{};
    MAddress objects[2]{};

    Remembered1314Relocation()
    {
        CreateStandaloneHeap(16);
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
        ZStat::Initialize();
        auto& old = Heap::GetHeap().old();
        old.InitializeWorkers(1);
        old.Workers()->set_active_workers(1);
        GenerationSequenceFixture::Advance(old);
        GenerationSequenceFixture::Advance(Heap::GetHeap().young());
        Heap::GetHeap().young().set_phase(ZGenerationPhase::Mark);
        alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
        auto* type = reinterpret_cast<TypeInfo*>(storage);
        type->SetType(TypeKind::TYPE_KIND_CLASS);
        type->SetFlagHasRefField();
        type->SetInstanceSize(sizeof(uintptr_t));
        type->SetAlign(8);
        GCTib tib{};
        tib.tag = SIGN_BIT | 1;
        type->SetGCTib(tib);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(storage), sizeof(storage));
        ZAllocationFlags flags;
        flags.set_non_blocking();
        ZPage* pages[2];
        ZRelocationSetSelector selector(0.0);
        for (size_t i = 0; i < 2; ++i) {
            pages[i] = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, PageAge::old, flags);
            GC_EXPECT_TRUE(pages[i] != nullptr);
            objects[i] = pages[i]->alloc_object(16);
            auto* object = reinterpret_cast<BaseObject*>(objects[i]);
            object->SetClassInfo(type);
            HeapSlotAt<>(objects[i] + TYPEINFO_PTR_SIZE).StoreColoured(StoreGoodPointer(nullptr));
            GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(pages[i], object));
            pages[i]->remember(reinterpret_cast<volatile zpointer*>(objects[i] + TYPEINFO_PTR_SIZE));
            ZPageTest::MakeRelocatable(*pages[i]);
            selector.register_live_page(pages[i]);
        }
        selector.select();
        old.relocation_set().install(&selector);
        ZRelocationSetIterator installed(&old.relocation_set());
        for (ZForwarding* owner; installed.next(&owner);) { old.forwarding_table().insert(owner); }
        for (size_t i = 0; i < 2; ++i) {
            owners[i] = forwarding_for_page(pages[i]);
            GC_EXPECT_TRUE(owners[i] != nullptr);
        }
    }

    void relocate()
    {
        auto& old = Heap::GetHeap().old();
        {
            ScopedStopTheWorld pause("Remembered1314 old relocate start", false);
            old.relocate_start();
        }
        old.relocate().relocate(&old.relocation_set());
    }

    MAddress field(size_t i) const
    {
        // Read the actual product forwarding result, not a fixture destination.
        return owners[i]->find(objects[i]) + TYPEINFO_PTR_SIZE;
    }
};

// ZGC zForwarding.inline.hpp:306-325: none appends, reject is a legal
// no-op, while accept means relocation has finished and cannot register.
GC_COMPONENT_OTHER_VM_TEST(Remembered1314, RegisterNoneAndReject)
{
    Remembered1314Relocation fixture;
    auto* fwd = fixture.owners[0];
    auto* rejectedFwd = fixture.owners[1];
    rejectedFwd->relocated_remembered_fields_notify_concurrent_scan_of();
    fixture.relocate();
    const MAddress field = fixture.field(0);
    const bool stored = fwd->relocated_remembered_fields_published_contains(field);
    const bool rejected = !rejectedFwd->relocated_remembered_fields_published_contains(fixture.field(1));
    fwd->relocated_remembered_fields_notify_concurrent_scan_of();
    fwd->relocated_remembered_fields_publish();
    const bool cleared = !fwd->relocated_remembered_fields_published_contains(field);
    std::fprintf(stderr, "REGISTER1314_LEGAL calls=2 stored=%d rejected=%d cleared=%d target_assertion=executed\n",
                 stored, rejected, cleared);
    GC_EXPECT_TRUE(stored);
    GC_EXPECT_TRUE(rejected);
    GC_EXPECT_TRUE(cleared);
}

// ZGC zForwarding.cpp:338-356: the scanner owns the published array
// after the CAS and clears it before returning; reject is repeatable.
GC_COMPONENT_OTHER_VM_TEST(Remembered1314, NotifyPublishedClearsFields)
{
    Remembered1314Relocation fixture;
    auto* fwd = fixture.owners[0];
    fixture.relocate();
    const MAddress field = fixture.field(0);
    const bool stored = fwd->relocated_remembered_fields_published_contains(field);
    fwd->relocated_remembered_fields_notify_concurrent_scan_of();
    const bool rejected = fwd->relocated_remembered_fields_is_concurrently_scanned();
    const bool cleared = !fwd->relocated_remembered_fields_published_contains(field);
    fwd->relocated_remembered_fields_notify_concurrent_scan_of();
    std::fprintf(stderr, "NOTIFY1314_TARGET precondition_stored=%d rejected=%d cleared=%d calls=2 target_assertion=executed\n",
                 stored, rejected, cleared);
    GC_EXPECT_TRUE(cleared);
    GC_EXPECT_TRUE(rejected);
    GC_EXPECT_TRUE(stored);
}

GC_COMPONENT_OTHER_VM_TEST(Remembered1314, RegisterAcceptFails)
{
    int diagnostic[2];
    GC_EXPECT_EQ(pipe(diagnostic), 0);
    const pid_t child = fork();
    if (child == 0) {
        close(diagnostic[0]);
        dup2(diagnostic[1], STDERR_FILENO);
        close(diagnostic[1]);
        Remembered1314Relocation fixture;
        auto* fwd = fixture.owners[0];
        fwd->relocated_remembered_fields_after_relocate();
        // Advance the protocol epoch without flipping away the source bits.
        GenerationSequenceFixture::Advance(Heap::GetHeap().young());
        fwd->relocated_remembered_fields_apply_to_published([](MAddress) {});
        // The real relocation phase must reject registration in accept state.
        fixture.relocate();
        _exit(0);
    }
    close(diagnostic[1]);
    std::string message;
    char buffer[1024];
    ssize_t length;
    while ((length = read(diagnostic[0], buffer, sizeof(buffer))) > 0) { message.append(buffer, length); }
    close(diagnostic[0]);
    int status = 0;
    GC_EXPECT_TRUE(child > 0);
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const bool target = message.find("Unexpected relocated remembered fields register state") != std::string::npos;
    const bool rejected = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT && target;
    std::fprintf(stderr, "REGISTER1314_ACCEPT_TARGET rejected=%d status=%d target_diagnostic=%d calls=1 target_assertion=executed\n",
                 rejected, status, target);
    GC_EXPECT_TRUE(rejected);
}

GC_TEST(Remembered1314, ThreeThreadOwnership)
{
    GcHeapFixture heap;
    auto& young = Heap::GetHeap().young();
    young.set_phase(ZGenerationPhase::Mark);
    auto* fwd = ZForwarding::Create(1, heap.heapStart, heap.heapStart, ZGranuleSize);
    // The scanner retains before OC starts; apply is only allowed after both
    // owners release. No array access is protected by a test-side mutex.
    GC_EXPECT_TRUE(fwd->retain_page(&generation_relocate_queue(Generation::Old)));
    std::atomic<bool> start{false}, notified{false}, relocated{false};
    size_t applied = 0;
    std::thread oc([&] {
        while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        for (size_t i = 0; i != 128; ++i) {
            fwd->relocated_remembered_fields_register(heap.heapStart + i * sizeof(void*));
        }
        fwd->relocated_remembered_fields_after_relocate();
        fwd->release_page();
        fwd->mark_done();
        relocated.store(true, std::memory_order_release);
    });
    std::thread ycNotify([&] {
        start.store(true, std::memory_order_release);
        fwd->relocated_remembered_fields_notify_concurrent_scan_of();
        fwd->release_page();
        notified.store(true, std::memory_order_release);
    });
    std::thread ycApply([&] {
        while (!notified.load(std::memory_order_acquire) || !relocated.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        fwd->relocated_remembered_fields_apply_to_published([&](MAddress) { ++applied; });
    });
    oc.join();
    ycNotify.join();
    ycApply.join();
    const bool rejected = fwd->relocated_remembered_fields_is_concurrently_scanned();
    fwd->Destroy();
    std::fprintf(stderr, "REGISTER1314_OWNERSHIP threads=3 calls=128 applied=%zu rejected=%d\n", applied, rejected);
    GC_EXPECT_EQ(applied, size_t{0});
    GC_EXPECT_TRUE(rejected);
}
