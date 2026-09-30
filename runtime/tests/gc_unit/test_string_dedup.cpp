// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// Private table observations are available only in the matching product configuration.
#if defined(MRT_TESTABLE_INTERNALS)
#include <cstdio>
#include <atomic>
#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include "CangjieRuntime.h"
#include "gc_worker_fixture.hpp"
#include "Common/ScopedObjectAccess.h"
#include "Common/SuspendibleThreadSet.h"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zResurrection.hpp"
#include "Heap/z/zRootsIterator.hpp"
#include "Mutator/Mutator.inline.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include "ObjectModel/MArray.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {
extern "C" ArrayRef MCC_NewArray8(const TypeInfo*, MIndex);
extern "C" ArrayRef MCC_StringDedupCanonicalImpl(const TypeInfo* arrayInfo, ArrayRef candidate);

class StringDedupTest {
public:
    static uint32_t Hash(MArray* array, uint64_t seed)
    {
        auto& dedup = StringDedup::Instance();
        const auto saved = dedup.hashSeed;
        dedup.hashSeed = seed;
        const uint32_t hash = dedup.Hash(array);
        dedup.hashSeed = saved;
        return hash;
    }
    static size_t Entries()
    {
        auto& dedup = StringDedup::Instance();
        std::lock_guard<std::mutex> lock(dedup.tableMutex);
        return dedup.table.numberOfEntries;
    }
    static void SetSeed(uint64_t seed)
    {
        auto& dedup = StringDedup::Instance();
        std::lock_guard<std::mutex> lock(dedup.tableMutex);
        dedup.hashSeed = seed;
    }
    static bool IsCleaning()
    {
        auto& dedup = StringDedup::Instance();
        std::lock_guard<std::mutex> lock(dedup.tableMutex);
        return dedup.table.cleanupState != nullptr;
    }
    static unsigned DeadState()
    {
        return static_cast<unsigned>(StringDedup::Instance().table.deadState.load(std::memory_order_acquire));
    }
    static size_t Buckets()
    {
        auto& dedup = StringDedup::Instance();
        std::lock_guard<std::mutex> lock(dedup.tableMutex);
        return dedup.table.numberOfBuckets;
    }
    static void ObserveOldReports(unsigned& firstState, unsigned& secondState)
    {
        auto& dedup = StringDedup::Instance();
        std::lock_guard<std::mutex> lock(dedup.tableMutex);
        ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED");
        ConcurrentGCBreakpoints::RunToIdle();
        firstState = DeadState();
        ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED");
        ConcurrentGCBreakpoints::RunToIdle();
        secondState = DeadState();
    }
};
}

namespace {
struct ByteArrays {
    GcHeapFixture heap;
    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)]{};
    MArray* first;
    MArray* second;
    ByteArrays()
    {
        StringDedup::Instance().Stop();
        StringDedup::Instance().Start();
        auto* component = reinterpret_cast<TypeInfo*>(componentStorage);
        component->SetType(TypeKind::TYPE_KIND_UINT8);
        component->SetInstanceSize(1);
        heap.typeInfo->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        heap.typeInfo->SetComponentTypeInfo(component);
        first = reinterpret_cast<MArray*>(heap.obj0);
        second = reinterpret_cast<MArray*>(heap.obj1);
        first->SetLength(2);
        second->SetLength(2);
    }
    ~ByteArrays() { StringDedup::Instance().Stop(); }
};
}

// TestStringDeduplicationTableResize: equal-length distinct values stay separate.
GC_TEST(StringDedup, SameLengthKeepsDistinctBacking)
{
    ByteArrays arrays;
    arrays.first->SetPrimitiveElement<I8>(0, 0);
    arrays.first->SetPrimitiveElement<I8>(1, 31);
    arrays.second->SetPrimitiveElement<I8>(0, 1);
    arrays.second->SetPrimitiveElement<I8>(1, 0);
    auto* left = MCC_StringDedupCanonicalImpl(arrays.heap.typeInfo, arrays.first);
    auto* right = MCC_StringDedupCanonicalImpl(arrays.heap.typeInfo, arrays.second);
    GC_EXPECT_TRUE(left == arrays.first);
    GC_EXPECT_TRUE(right == arrays.second);
    GC_EXPECT_EQ(StringDedupTest::Entries(), 2U);
}

// TestStringDeduplication: equal values share one weak table entry, and the
// second call returns that entry (stringDedupTable.cpp:634).
GC_TEST(StringDedup, ExplicitEqualStringBackingFindsEntry)
{
    ByteArrays arrays;
    for (auto* array : {arrays.first, arrays.second}) {
        array->SetPrimitiveElement<I8>(0, 7);
        array->SetPrimitiveElement<I8>(1, 9);
    }
    auto* first = MCC_StringDedupCanonicalImpl(arrays.heap.typeInfo, arrays.first);
    auto* second = MCC_StringDedupCanonicalImpl(arrays.heap.typeInfo, arrays.second);
    GC_EXPECT_EQ(StringDedupTest::Entries(), 1U);
    GC_EXPECT_TRUE(first == arrays.first);
    const int hit = second == arrays.first ? 1 : 0;
    std::printf("STRING_DEDUP_CANONICAL_HIT second_is_first=%d\n", hit);
    std::fflush(stdout);
    GC_EXPECT_TRUE(second == arrays.first);
}

// zRootsIterator.cpp:169-176 and zMark.cpp:853-867 (ZGC zRootsIterator.cpp:
//194-199 AllColored includes _oop_storage_set_weak): a dedup entry lives in the
// weak OopStorage set, not the strong set, and the young colored-root pass
// walks it. This checks root routing; the cycle test checks backing identity.
GC_TEST(StringDedup, DedupWeakStorageIsYoungColoredRoot)
{
    ByteArrays arrays;
    arrays.first->SetPrimitiveElement<I8>(0, 0x35);
    arrays.first->SetPrimitiveElement<I8>(1, 0x24);
    auto* installed = MCC_StringDedupCanonicalImpl(arrays.heap.typeInfo, arrays.first);
    // Construction evidence, kept non-fatal so it cannot mask the target line.
    std::printf("STRING_DEDUP_YOUNG_ROOT installed=%d entries=%zu\n", installed == arrays.first ? 1 : 0,
                StringDedupTest::Entries());
    std::fflush(stdout);
    BaseObject* target = installed;
    unsigned visited = 0;
    unsigned weakVisited = 0;
    unsigned strongVisited = 0;
    OopStorageSetIteratorWeak weak;
    weak.Apply([&](NativeSlot& slot) {
        if (NativeAccess<ON_PHANTOM_OOP_REF | AS_NO_KEEPALIVE>::oop_load(&slot) == target) ++weakVisited;
    });
    OopStorageSetIteratorStrong strong;
    strong.Apply([&](NativeSlot& slot) {
        if (NativeAccess<>::oop_load(&slot) == target) ++strongVisited;
    });
    RootsIteratorAllColored colored;
    colored.Apply([&](NativeSlot& slot) {
        if (NativeAccess<>::oop_load(&slot) == target) ++visited;
    });
    std::printf("STRING_DEDUP_YOUNG_COLORED_ROOT visited=%u weak=%u strong=%u\n",
                visited, weakVisited, strongVisited);
    std::fflush(stdout);
    GC_EXPECT_EQ(visited, 1U);
    GC_EXPECT_EQ(weakVisited, 1U);
    GC_EXPECT_EQ(strongVisited, 0U);
    GC_EXPECT_TRUE(installed == arrays.first);
}

// TestStringDeduplicationYoungGC adaptation: only explicit String ABI requests
// are supported; malformed backing is rejected before byte consumption.
GC_TEST(StringDedup, RejectOrdinaryObject)
{
    GcHeapFixture heap;
    auto& dedup = StringDedup::Instance();
    dedup.Stop();
    dedup.Start();
    auto* rejected = MCC_StringDedupCanonicalImpl(heap.typeInfo, reinterpret_cast<ArrayRef>(heap.obj0));
    GC_EXPECT_TRUE(rejected == reinterpret_cast<ArrayRef>(heap.obj0));
    GC_EXPECT_EQ(StringDedupTest::Entries(), 0U);
    dedup.Stop();
}

GC_TEST(StringDedup, EmptyLengthNotInstalled)
{
    ByteArrays arrays;
    arrays.first->SetLength(0);
    auto* returned = MCC_StringDedupCanonicalImpl(arrays.heap.typeInfo, arrays.first);
    GC_EXPECT_TRUE(returned == arrays.first);
    GC_EXPECT_EQ(StringDedupTest::Entries(), 0U);
}

GC_TEST(StringDedup, NullCandidateReturned)
{
    ByteArrays arrays;
    auto* returned = MCC_StringDedupCanonicalImpl(arrays.heap.typeInfo, nullptr);
    GC_EXPECT_TRUE(returned == nullptr);
    GC_EXPECT_EQ(StringDedupTest::Entries(), 0U);
}

// AltHashingTest.halfsiphash_test_ByteArray: upstream aggregate reference vector.
GC_TEST(StringDedup, HalfSipHashByteArrayReference)
{
    ByteArrays arrays;
    uint8_t hashes[1024];
    uint8_t* bytes = arrays.first->ConvertToCArray();
    for (unsigned i = 0; i < 256; ++i) bytes[i] = static_cast<uint8_t>(i);
    for (unsigned length = 0; length < 256; ++length) {
        arrays.first->SetLength(length);
        const uint32_t hash = StringDedupTest::Hash(arrays.first, 256 - length);
        for (unsigned byte = 0; byte != 4; ++byte) hashes[length * 4 + byte] = hash >> (byte * 8);
    }
    std::memcpy(bytes, hashes, sizeof(hashes));
    arrays.first->SetLength(sizeof(hashes));
    GC_EXPECT_EQ(StringDedupTest::Hash(arrays.first, 0), 0xd2be7fd8U);
}

namespace {
struct DedupByteType {
    alignas(TypeInfo) unsigned char data[2][sizeof(TypeInfo)]{};
    TypeInfo* array;
    DedupByteType()
    {
        auto* byte = reinterpret_cast<TypeInfo*>(data[0]);
        array = reinterpret_cast<TypeInfo*>(data[1]);
        byte->SetType(TypeKind::TYPE_KIND_UINT8);
        byte->SetInstanceSize(1);
        array->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        array->SetComponentTypeInfo(byte);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(data), sizeof(data));
    }
};
TypeInfo* DedupArrayType() { static DedupByteType type; return type.array; }

// C++ input carrier only: all allocation, deduplication and collection use
// the linked product. None of these native pointers are registered as roots.
struct DedupCycle {
    bool old = false;
    bool strongControl = false;
    bool different = false;
    bool collision = false;
    bool hashesEqual = false;
    std::atomic<unsigned> stage{0};
    std::atomic<unsigned> command{0};
    BaseObject* original = nullptr;
    BaseObject* returned = nullptr;
    NativeSlot* candidate = nullptr;
    NativeSlot* control = nullptr;
    bool installed = false;
    bool hit = false;
    bool watermarkDone = false;
    bool wasMarked = false;
    bool blocked = false;
    bool preparedOld = false;
    ZPage* originalPage = nullptr;
    MArray* candidateBacking = nullptr;
};
void WaitCommand(DedupCycle& cycle, unsigned command)
{
    ScopedEnterSaferegion safe(false);
    cycle.stage.store(command, std::memory_order_release);
    while (cycle.command.load(std::memory_order_acquire) < command) std::this_thread::yield();
}
MArray* NewCycleBacking(bool old, uint64_t content)
{
    auto* type = DedupArrayType();
    auto* value = old ? reinterpret_cast<MArray*>(Heap::GetHeap().object_allocator().alloc_for_relocation(
                          sizeof(MArray) + sizeof(content), PageAge::old)) : MCC_NewArray8(type, sizeof(content));
    value->SetClassInfo(type);
    value->SetLength(sizeof(content));
    std::memcpy(value->ConvertToCArray(), &content, sizeof(content));
    return value;
}
void* DedupCycleTask(void* argument)
{
    auto& cycle = *static_cast<DedupCycle*>(argument);
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    auto& storage = Heap::GetHeap().GetFinalizerProcessor().StrongRootStorage();
    auto* first = NewCycleBacking(cycle.old, cycle.collision ? 129171 : 0x13120001);
    cycle.original = first;
    cycle.originalPage = Heap::page(reinterpret_cast<uintptr_t>(first));
    cycle.preparedOld = !Heap::page(reinterpret_cast<uintptr_t>(first))->IsYoungRegion();
    cycle.installed = MCC_StringDedupCanonicalImpl(DedupArrayType(), first) == first;
    if (cycle.strongControl) {
        cycle.control = storage.Allocate();
        NativeAccess<>::oop_store(cycle.control, first);
    }
    auto* second = NewCycleBacking(cycle.old, cycle.collision ? 187275 : (cycle.different ? 0x13120002 : 0x13120001));
    cycle.candidateBacking = second;
    if (cycle.collision) cycle.hashesEqual = StringDedupTest::Hash(first, 0) == StringDedupTest::Hash(second, 0);
    cycle.candidate = storage.Allocate();
    NativeAccess<>::oop_store(cycle.candidate, second);
    WaitCommand(cycle, 1);
    cycle.watermarkDone = mutator->GetStackWatermark().IsDone(StackWatermark::epoch_id());
    cycle.blocked = ZResurrection::is_blocked();
    if (cycle.old) {
        cycle.wasMarked = Heap::page(reinterpret_cast<uintptr_t>(cycle.original))->is_object_marked(
            from_object(cycle.original), false);
    }
    second = static_cast<MArray*>(NativeAccess<>::oop_load(cycle.candidate));
    cycle.returned = MCC_StringDedupCanonicalImpl(DedupArrayType(), second);
    // B has never been installed before this lookup: the only other possible
    // matching identity is A, even if a minor cycle has moved A.
    cycle.hit = cycle.returned != second;
    WaitCommand(cycle, 2);
    for (NativeSlot* slot : {cycle.candidate, cycle.control}) {
        if (slot != nullptr) { NativeAccess<>::oop_store(slot, nullptr); storage.Release(slot); }
    }
    mutator->SetManagedContext(true);
    return nullptr;
}
bool WaitStage(DedupCycle& cycle, unsigned stage)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cycle.stage.load(std::memory_order_acquire) < stage && std::chrono::steady_clock::now() < end) {
        std::this_thread::yield();
    }
    return cycle.stage.load(std::memory_order_acquire) >= stage;
}
void StartDedupRuntime()
{
    RuntimeParam params{};
    params.heapParam.heapSize = 128 * 1024;
    params.coParam.processorNum = 2;
    params.gcParam.concGCThreads = 4;
    params.gcParam.concGCThreadsSet = true;
    params.gcParam.youngGCThreads = 2;
    params.gcParam.oldGCThreads = 2;
    params.gcParam.youngGCThreadsSet = true;
    params.gcParam.oldGCThreadsSet = true;
    params.gcParam.staticGCThreads = true;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
}
void CheckDedupCycle(bool old, bool strongControl, bool blocked, bool different, bool collision = false)
{
    StartDedupRuntime();
    ConcurrentGCBreakpoints::AcquireControl();
    DedupCycle cycle;
    cycle.old = old;
    cycle.strongControl = strongControl;
    cycle.different = different;
    cycle.collision = collision;
    if (collision) StringDedupTest::SetSeed(0);
    auto task = RunCJTask(DedupCycleTask, &cycle);
    const bool prepared = task != nullptr && WaitStage(cycle, 1);
    bool reached = prepared;
    if (prepared) {
        if (old) reached = ConcurrentGCBreakpoints::RunTo(blocked ?
            "AFTER CONCURRENT REFERENCE PROCESSING STARTED" : "BEFORE MARKING COMPLETED");
        else Heap::GetHeap().RequestGC(GC_REASON_YOUNG);
    }
    cycle.command.store(1, std::memory_order_release);
    const bool called = task != nullptr && WaitStage(cycle, 2);
    if (old && !blocked && reached) {
        reached = ConcurrentGCBreakpoints::RunTo("AFTER CONCURRENT REFERENCE PROCESSING STARTED");
    }
    const bool markedAtEnd = old && prepared && Heap::page(reinterpret_cast<uintptr_t>(cycle.original))->
        is_object_marked(from_object(cycle.original), false);
    // Target evidence precedes *all* assertions; preparation failures cannot
    // hide a target assertion behind a fatal EXPECT.
    std::printf("DEDUP_CYCLE_TARGET old=%d strong=%d blocked=%d different=%d prepared=%d installed=%d "
                "old_page=%d called=%d watermark_done=%d before_marked=%d hit=%d marked_at_end=%d\n",
                old, strongControl, blocked, different, prepared, cycle.installed, cycle.preparedOld,
                called, cycle.watermarkDone, cycle.wasMarked, cycle.hit, markedAtEnd);
    std::fflush(stdout);
    if (old && reached) ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    cycle.command.store(2, std::memory_order_release);
    void* result = nullptr;
    const int taskRC = task == nullptr ? -1 : GetTaskRet(task, &result);
    if (task != nullptr) ReleaseHandle(task);
    const int finiRC = FiniCJRuntime();
    // Assert the observable product result first, then separately validate
    // the construction (including that the expected old object was unmarked).
    if (old && !blocked && !different) GC_EXPECT_TRUE(markedAtEnd);
    if (old && different) GC_EXPECT_EQ(markedAtEnd, strongControl);
    if (collision) GC_EXPECT_TRUE(cycle.hashesEqual);
    GC_EXPECT_EQ(cycle.hit, !different && (!blocked || strongControl));
    GC_EXPECT_TRUE(prepared && called && reached && cycle.installed);
    if (old) {
        GC_EXPECT_TRUE(cycle.preparedOld && cycle.watermarkDone);
        GC_EXPECT_EQ(cycle.blocked, blocked);
        GC_EXPECT_EQ(cycle.wasMarked, strongControl);
    }
    GC_EXPECT_EQ(taskRC, E_OK);
    GC_EXPECT_EQ(finiRC, E_OK);
}
}

GC_RUNTIME_OTHER_VM_TEST(StringDedup, YoungTableRootKeepsIdentity)
{
    CheckDedupCycle(false, false, false, false);
}
GC_RUNTIME_OTHER_VM_TEST(StringDedup, YoungStrongRootControl)
{
    CheckDedupCycle(false, true, false, false);
}
GC_RUNTIME_OTHER_VM_TEST(StringDedup, YoungDifferentContentControl)
{
    CheckDedupCycle(false, false, false, true);
}
GC_RUNTIME_OTHER_VM_TEST(StringDedup, OldHitKeepsAliveAtMarkEnd)
{
    CheckDedupCycle(true, false, false, false);
}
GC_RUNTIME_OTHER_VM_TEST(StringDedup, OldStrongRootControl)
{
    CheckDedupCycle(true, true, false, false);
}
GC_RUNTIME_OTHER_VM_TEST(StringDedup, BlockedDeadEntryIsMiss)
{
    CheckDedupCycle(true, false, true, false);
}
GC_RUNTIME_OTHER_VM_TEST(StringDedup, BlockedStrongRootControl)
{
    CheckDedupCycle(true, true, true, false);
}
GC_RUNTIME_OTHER_VM_TEST(StringDedup, OldDifferentContentControl)
{
    CheckDedupCycle(true, false, false, true);
}

GC_RUNTIME_OTHER_VM_TEST(StringDedup, NonMatchingPeekDoesNotKeepAlive)
{
    CheckDedupCycle(true, false, false, true, true);
}
GC_RUNTIME_OTHER_VM_TEST(StringDedup, NonMatchingStrongRootControl)
{
    CheckDedupCycle(true, true, false, true, true);
}

namespace {
struct DedupBatch {
    size_t count = 0;
    size_t strongCount = 0;
    size_t installed = 0;
    size_t duplicateHits = 0;
    std::vector<NativeSlot*> strong;
};
void* InstallDedupBatch(void* argument)
{
    auto& batch = *static_cast<DedupBatch*>(argument);
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    auto& storage = Heap::GetHeap().GetFinalizerProcessor().StrongRootStorage();
    for (size_t index = 0; index < batch.count; ++index) {
        auto* array = NewCycleBacking(true, index + 1);
        batch.installed += MCC_StringDedupCanonicalImpl(DedupArrayType(), array) == array;
        // Revisit older keys while the independent processor can migrate
        // buckets. A miss would install a duplicate and change slot count.
        if (batch.count > 503 * 14 && index % 64 == 0) {
            auto* same = NewCycleBacking(true, 1);
            batch.duplicateHits += MCC_StringDedupCanonicalImpl(DedupArrayType(), same) != same;
        }
        if (index < batch.strongCount) {
            auto* slot = storage.Allocate();
            NativeAccess<>::oop_store(slot, array);
            batch.strong.push_back(slot);
        }
    }
    mutator->SetManagedContext(true);
    return nullptr;
}
void* ReleaseDedupBatch(void* argument)
{
    auto& batch = *static_cast<DedupBatch*>(argument);
    auto& storage = Heap::GetHeap().GetFinalizerProcessor().StrongRootStorage();
    for (auto* slot : batch.strong) { NativeAccess<>::oop_store(slot, nullptr); storage.Release(slot); }
    return nullptr;
}
void RunDedupTask(void* (*entry)(void*), void* argument)
{
    auto task = RunCJTask(entry, argument);
    GC_EXPECT_TRUE(task != nullptr);
    void* result = nullptr;
    const int rc = GetTaskRet(task, &result);
    ReleaseHandle(task);
    GC_EXPECT_EQ(rc, E_OK);
}
bool WaitDedupSize(size_t count, bool requireGrowthComplete = false)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < end) {
        // Installed size can be reached before the asynchronous processor starts.
        // ZGC stringDedupTable.cpp:650-720: wait2 is published after resizing.
        if ((!requireGrowthComplete ||
             (StringDedupTest::Buckets() > 503 && StringDedupTest::DeadState() == 2U)) &&
            !StringDedupTest::IsCleaning() && StringDedupTest::Entries() == count &&
            StringDedup::Instance().WeakStorage().AllocationCount() == count) return true;
        std::this_thread::yield();
    }
    return false;
}
void DedupOldCycle()
{
    const bool reached = ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED");
    ConcurrentGCBreakpoints::RunToIdle();
    GC_EXPECT_TRUE(reached);
}
void CheckDeadCleanup(bool noDead)
{
    StartDedupRuntime();
    ConcurrentGCBreakpoints::AcquireControl();
    DedupBatch batch;
    batch.count = 160;
    batch.strongCount = noDead ? batch.count : 3;
    RunDedupTask(InstallDedupBatch, &batch);
    const size_t before = StringDedup::Instance().WeakStorage().AllocationCount();
    auto& otherOwner = Heap::GetHeap().GetFinalizerProcessor();
    const size_t otherBefore = otherOwner.WeakRootStorage().AllocationCount();
    const size_t otherDead = noDead ? 0 : 7;
    for (size_t index = 0; index < otherDead; ++index) otherOwner.RegisterFinalizer(nullptr);
    const size_t otherRegistered = otherOwner.WeakRootStorage().AllocationCount();
    DedupOldCycle();
    const size_t otherAfter = otherOwner.WeakRootStorage().AllocationCount();
    const bool settled = WaitDedupSize(batch.strongCount);
    const size_t entries = StringDedupTest::Entries();
    const size_t slots = StringDedup::Instance().WeakStorage().AllocationCount();
    std::printf("DEDUP_OLD_CLEAN_TARGET installed=%zu before=%zu entries=%zu slots=%zu expected=%zu settled=%d "
                "other_before=%zu other_registered=%zu other_after=%zu\n",
        batch.installed, before, entries, slots, batch.strongCount, settled,
        otherBefore, otherRegistered, otherAfter);
    std::fflush(stdout);
    RunDedupTask(ReleaseDedupBatch, &batch);
    ConcurrentGCBreakpoints::ReleaseControl();
    const int finiRC = FiniCJRuntime();
    GC_EXPECT_EQ(entries, batch.strongCount);
    GC_EXPECT_EQ(slots, batch.strongCount);
    GC_EXPECT_TRUE(settled);
    GC_EXPECT_EQ(before, batch.count);
    GC_EXPECT_EQ(otherRegistered, otherBefore + otherDead);
    GC_EXPECT_EQ(otherAfter, otherBefore);
    GC_EXPECT_EQ(batch.installed, batch.count);
    GC_EXPECT_EQ(finiRC, E_OK);
}
}
GC_RUNTIME_OTHER_VM_TEST(StringDedup, OldClearReportsAndCleans)
{
    CheckDeadCleanup(false);
}
GC_RUNTIME_OTHER_VM_TEST(StringDedup, OldNoDeadControl)
{
    CheckDeadCleanup(true);
}
GC_RUNTIME_OTHER_VM_TEST(StringDedup, ResizeThenOldCallbacksShrink)
{
    StartDedupRuntime();
    ConcurrentGCBreakpoints::AcquireControl();
    DedupBatch batch;
    batch.count = 7200;
    RunDedupTask(InstallDedupBatch, &batch);
    const bool grown = WaitDedupSize(batch.count, true);
    const size_t grownBuckets = StringDedupTest::Buckets();
    const unsigned grownState = StringDedupTest::DeadState();
    DedupOldCycle();
    const unsigned firstState = StringDedupTest::DeadState();
    DedupOldCycle();
    const bool cleaned = WaitDedupSize(0);
    const size_t finalBuckets = StringDedupTest::Buckets();
    const size_t finalSlots = StringDedup::Instance().WeakStorage().AllocationCount();
    std::printf("DEDUP_RESIZE_TARGET installed=%zu grown=%d buckets=%zu state=%u first_state=%u "
                "cleaned=%d final_buckets=%zu final_slots=%zu duplicate_hits=%zu expected_hits=%zu\n",
                batch.installed, grown, grownBuckets, grownState, firstState, cleaned, finalBuckets, finalSlots,
                batch.duplicateHits, (batch.count + 63) / 64);
    std::fflush(stdout);
    ConcurrentGCBreakpoints::ReleaseControl();
    const int finiRC = FiniCJRuntime();
    GC_EXPECT_TRUE(grownBuckets > 503);
    GC_EXPECT_EQ(batch.duplicateHits, (batch.count + 63) / 64);
    GC_EXPECT_TRUE(cleaned);
    GC_EXPECT_EQ(finalBuckets, size_t{503});
    GC_EXPECT_EQ(finalSlots, size_t{0});
    GC_EXPECT_EQ(grownState, 2U); // wait2 -> wait1 -> good through old reports
    GC_EXPECT_EQ(firstState, 1U);
    GC_EXPECT_TRUE(grown);
    GC_EXPECT_EQ(finiRC, E_OK);
}

namespace {
bool RunMaintenanceSchedule(const char* fixture)
{
    if (std::getenv("DEDUP_MAINTENANCE_FILE") != nullptr) return false;
    char executable[4096]{};
    const auto length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    GC_EXPECT_TRUE(length > 0);
    const auto source = std::filesystem::absolute(__FILE__);
    const auto scheduler = source.parent_path() / "test_string_dedup_maintenance_gdb.py";
    GC_EXPECT_TRUE(std::filesystem::is_regular_file(scheduler));
    const std::string prefix = std::string(executable) + ".maintenance-" + std::to_string(getpid());
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        setenv("DEDUP_MAINTENANCE_FILE", prefix.c_str(), 1);
        setenv("DEDUP_MAINTENANCE_FIXTURE", fixture, 1);
        setenv("DEDUP_MAINTENANCE_SOURCE", source.c_str(), 1);
        execlp("timeout", "timeout", "90", "gdb", "-nx", "-batch", "-x", scheduler.c_str(),
               "--args", executable, static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    const auto waited = waitpid(child, &status, 0);
    GC_EXPECT_TRUE(waited == child && WIFEXITED(status));
    GC_EXPECT_EQ(WEXITSTATUS(status), 0);
    return true;
}
bool WaitMaintenanceWindow()
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    const std::string prefix = std::getenv("DEDUP_MAINTENANCE_FILE");
    while (std::chrono::steady_clock::now() < end) {
        if (std::filesystem::exists(prefix + ".ready")) return true;
        std::this_thread::yield();
    }
    return false;
}
void ReleaseMaintenanceWindow()
{
    const std::string prefix = std::getenv("DEDUP_MAINTENANCE_FILE");
    std::ofstream(prefix + ".release") << "release\n";
}
struct OldBucketProbe {
    ArrayRef found = nullptr;
    ArrayRef expected = nullptr;
};
void* ProbeOldBucket(void* argument)
{
    auto& probe = *static_cast<OldBucketProbe*>(argument);
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    auto* candidate = NewCycleBacking(true, 1);
    probe.found = MCC_StringDedupCanonicalImpl(DedupArrayType(), candidate);
    mutator->SetManagedContext(true);
    return nullptr;
}
}
GC_RUNTIME_OTHER_VM_TEST(StringDedup, CleanupReportStateTransitions)
{
    if (RunMaintenanceSchedule("StringDedup.CleanupReportStateTransitions")) return;
    StartDedupRuntime();
    ConcurrentGCBreakpoints::AcquireControl();
    DedupBatch batch;
    batch.count = 7200;
    RunDedupTask(InstallDedupBatch, &batch);
    const bool window = WaitMaintenanceWindow();
    const unsigned cleaningState = StringDedupTest::DeadState();
    ReleaseMaintenanceWindow();
    const bool grown = WaitDedupSize(batch.count, true);
    const unsigned completedState = StringDedupTest::DeadState();
    unsigned firstState = 99;
    unsigned secondState = 99;
    StringDedupTest::ObserveOldReports(firstState, secondState);
    std::printf("DEDUP_STATE_TARGET window=%d grown=%d cleaning=%u completed=%u first=%u second=%u\n",
                window, grown, cleaningState, completedState, firstState, secondState);
    std::fflush(stdout);
    ConcurrentGCBreakpoints::ReleaseControl();
    const int finiRC = FiniCJRuntime();
    GC_EXPECT_TRUE(cleaningState == 3U && completedState == 2U && firstState == 1U && secondState == 0U);
    GC_EXPECT_TRUE(window && grown);
    GC_EXPECT_EQ(finiRC, E_OK);
}
GC_RUNTIME_OTHER_VM_TEST(StringDedup, ShrinkingOldBucketKeepsCanonicalIdentity)
{
    if (RunMaintenanceSchedule("StringDedup.ShrinkingOldBucketKeepsCanonicalIdentity")) return;
    StartDedupRuntime();
    ConcurrentGCBreakpoints::AcquireControl();
    DedupBatch batch;
    batch.count = 7200;
    batch.strongCount = 3;
    RunDedupTask(InstallDedupBatch, &batch);
    const bool grown = WaitDedupSize(batch.count, true);
    DedupOldCycle();
    DedupOldCycle();
    const bool window = WaitMaintenanceWindow();
    OldBucketProbe probe;
    probe.expected = reinterpret_cast<ArrayRef>(NativeAccess<>::oop_load(batch.strong.front()));
    RunDedupTask(ProbeOldBucket, &probe);
    const size_t slots = StringDedup::Instance().WeakStorage().AllocationCount();
    std::printf("DEDUP_SHRINK_TARGET window=%d grown=%d found=%p expected=%p slots=%zu\n",
                window, grown, probe.found, probe.expected, slots);
    std::fflush(stdout);
    ReleaseMaintenanceWindow();
    RunDedupTask(ReleaseDedupBatch, &batch);
    ConcurrentGCBreakpoints::ReleaseControl();
    const int finiRC = FiniCJRuntime();
    GC_EXPECT_TRUE(probe.found == probe.expected);
    GC_EXPECT_TRUE(window && grown);
    GC_EXPECT_EQ(finiRC, E_OK);
}

// Component lock-order evidence only. The forwarding/marking inputs below
// are explicit; real GC creation of these inputs is covered by cycle tests.
// The waiter uses the real table/barrier/queue; only product ForwardTask
// publishes the moved object and completes its page.
GC_OTHER_VM_TEST(StringDedup, RelocationWaitAllowsWorkerAndStop)
{
    // Queue observations belong to the debugger, not to a product-only
    // PendingCount API (ZGC zRelocate.cpp:134-191). Run this same fixture under
    // the read-only observer; all original behavioral assertions remain below.
    if (std::getenv("DEDUP_QUEUE_OBSERVER_FILE") == nullptr) {
        char executable[4096]{};
        const auto length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
        GC_EXPECT_TRUE(length > 0);
        executable[length] = '\0';
        const std::string snapshot = std::string(executable) + ".queue-observation-" + std::to_string(getpid());
        std::filesystem::path source = __FILE__;
        if (source.is_relative()) {
            // ccache can record a source path relative to the lane root. A
            // cut runs from its own build directory, so resolve from the ELF.
            auto directory = std::filesystem::path(executable).parent_path();
            while (!directory.empty()) {
                if (std::filesystem::is_regular_file(directory / source)) {
                    source = directory / source;
                    break;
                }
                const auto parent = directory.parent_path();
                if (parent == directory) break;
                directory = parent;
            }
        }
        source = std::filesystem::absolute(source);
        const auto observer = source.parent_path() / "test_string_dedup_queue_gdb.py";
        GC_EXPECT_TRUE(std::filesystem::is_regular_file(source));
        GC_EXPECT_TRUE(std::filesystem::is_regular_file(observer));
        const pid_t child = fork();
        GC_EXPECT_TRUE(child >= 0);
        if (child == 0) {
            setenv("DEDUP_QUEUE_OBSERVER_FILE", snapshot.c_str(), 1);
            setenv("DEDUP_QUEUE_OBSERVER_SOURCE", source.c_str(), 1);
            execlp("gdb", "gdb", "-nx", "-batch", "-x", observer.c_str(),
                   "--args", executable, static_cast<char*>(nullptr));
            _exit(127);
        }
        int status = 0;
        const auto waited = waitpid(child, &status, 0);
        std::ifstream receipt(snapshot + ".receipt");
        std::string qualification;
        receipt >> qualification;
        unlink(snapshot.c_str());
        unlink((snapshot + ".receipt").c_str());
        GC_EXPECT_TRUE(waited == child && WIFEXITED(status));
        GC_EXPECT_EQ(WEXITSTATUS(status), 0);
        GC_EXPECT_TRUE(qualification == "qualified");
        return;
    }
    ByteArrays arrays;
    auto& heap = Heap::GetHeap();
    auto& old = heap.old();
    auto* page = arrays.heap.region0();
    auto* companion = ZPage::InitRegion(ZPage::GranuleIndex(arrays.heap.heapStart) + 2,
                                       ZGranuleSize, ZPageType::small);
    PublishAllocatedPage(companion);
    ZPageTest::MakeRelocatable(*companion);
    auto* extra = reinterpret_cast<MArray*>(arrays.heap.PlaceObject(companion->GetRegionStart() + 64));
    extra->SetLength(2);
    extra->SetPrimitiveElement<I8>(0, 19);
    extra->SetPrimitiveElement<I8>(1, 23);
    companion->SetRegionAllocPtr(reinterpret_cast<uintptr_t>(extra) + extra->GetSize());
    for (auto* array : {arrays.first, arrays.second}) {
        array->SetPrimitiveElement<I8>(0, 7);
        array->SetPrimitiveElement<I8>(1, 9);
    }
    const bool installed = MCC_StringDedupCanonicalImpl(arrays.heap.typeInfo, arrays.first) == arrays.first;
    GcHeapFixture::MarkStrong(page, arrays.first);
    GcHeapFixture::MarkStrong(companion, extra);
    BeginForwardingArena(Generation::Old, {page, companion});
    auto* owner = ZGeneration::generation((page)->generation_id())->forwarding((page)->GetRegionStart());
    GC_EXPECT_TRUE(owner != nullptr); // Input qualification, before starting threads.
    // A negative page lease count is the product retain_page wait condition.
    // Task ownership stays available for the real queue worker to claim.
    owner->in_place_relocation_claim_page();
    if (old.Workers() == nullptr) MapleRuntime::GcUnit::InitializeGenerationWorkers(old, 1);
    old.Workers()->set_active_workers(1);
    old.Workers()->set_active();
    ZGlobalsPointers::flip_old_relocate_start();
    old.set_phase(ZGeneration::Phase::Relocate);
    ZRelocate::StartRelocationTasks(old.id());
    auto& queue = *old.relocate().queue();
    const auto pendingCount = [&] {
        // All threads are frozen at this source line. GDB reads queue.queue._len
        // and writes only its result file; this code does not read private data.
        std::ifstream snapshot(std::getenv("DEDUP_QUEUE_OBSERVER_FILE")); // DEDUP_QUEUE_SNAPSHOT
        size_t count = 0;
        GC_EXPECT_TRUE(static_cast<bool>(snapshot >> count));
        return count;
    };
    std::atomic<bool> finished{false};
    MArray* answer = nullptr;
    std::thread waiter([&] {
        // Same rendezvous membership as the maintenance step. No callback,
        // replacement barrier, table result or forwarding entry is injected.
        SuspendibleThreadSetJoiner joined;
        answer = MCC_StringDedupCanonicalImpl(arrays.heap.typeInfo, arrays.second);
        finished.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (pendingCount() == 0 && !finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool queued = pendingCount() == 1 && !finished.load(std::memory_order_acquire);
    std::atomic<bool> stopEntered{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> rendezvousDone{false};
    std::thread stopper([&] {
        stopEntered.store(true, std::memory_order_release);
        StringDedup::Instance().Stop();
        stopped.store(true, std::memory_order_release);
    });
    std::thread rendezvous([&] {
        SuspendibleThreadSet::synchronize();
        SuspendibleThreadSet::desynchronize();
        rendezvousDone.store(true, std::memory_order_release);
    });
    while ((!stopEntered.load(std::memory_order_acquire) || !SuspendibleThreadSet::should_yield()) &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool waitingForPublication = queued && SuspendibleThreadSet::should_yield() &&
        !stopped.load(std::memory_order_acquire) && !rendezvousDone.load(std::memory_order_acquire);
    // This is the real dispatcher/ForwardTask path, not a hand-written insert.
    // It must progress while both the table owner and STS rendezvous wait.
    old.relocate().relocate(&old.relocation_set());
    old.Workers()->set_inactive();
    waiter.join();
    stopper.join();
    rendezvous.join();
    const bool returned = answer != nullptr && answer != arrays.second && answer != arrays.first &&
        answer->GetLength() == 2 && answer->GetPrimitiveElement<I8>(0) == 7 &&
        answer->GetPrimitiveElement<I8>(1) == 9;
    const bool done = owner->is_done() && pendingCount() == 0;
    const size_t remaining = StringDedup::Instance().WeakStorage().AllocationCount();
    std::printf("DEDUP_WAIT_TARGET installed=%d queued=%d returned=%d done=%d stopped_slots=%zu "
                "waiting_for_publication=%d stopped=%d rendezvous_done=%d\n", installed, queued, returned,
                done, remaining, waitingForPublication, stopped.load(), rendezvousDone.load());
    std::fflush(stdout);
    GC_EXPECT_TRUE(queued && waitingForPublication);
    GC_EXPECT_TRUE(returned && done && stopped.load() && rendezvousDone.load());
    GC_EXPECT_EQ(remaining, size_t{0});
    GC_EXPECT_TRUE(installed);
}

#endif // MRT_TESTABLE_INTERNALS
