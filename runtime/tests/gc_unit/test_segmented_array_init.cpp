// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#if defined(MRT_GC_UNIT_TESTS)

#include <cstdlib>
#include <cstring>
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include "Heap/Collector/GcStats.h"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/MArray.inline.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
extern "C" ArrayRef MCC_NewObjArray(const TypeInfo* arrayInfo, MIndex nElems);
extern "C" ArrayRef MCC_NewArray8(const TypeInfo* arrayInfo, MIndex nElems);
}

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
struct ReferenceArrayTypeInfos {
    ReferenceArrayTypeInfos()
    {
        std::memset(componentStorage, 0, sizeof(componentStorage));
        component = reinterpret_cast<TypeInfo*>(componentStorage);
        component->SetType(TypeKind::TYPE_KIND_CLASS);
        component->SetInstanceSize(sizeof(void*));

        std::memset(arrayStorage, 0, sizeof(arrayStorage));
        array = reinterpret_cast<TypeInfo*>(arrayStorage);
        array->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        array->SetComponentTypeInfo(component);

        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(this), sizeof(*this));
    }

    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)];
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)];
    TypeInfo* component = nullptr;
    TypeInfo* array = nullptr;
};

ReferenceArrayTypeInfos& GetReferenceArrayTypeInfos()
{
    static ReferenceArrayTypeInfos infos;
    return infos;
}

struct ByteArrayTypeInfos {
    ByteArrayTypeInfos()
    {
        std::memset(componentStorage, 0, sizeof(componentStorage));
        component = reinterpret_cast<TypeInfo*>(componentStorage);
        component->SetType(TypeKind::TYPE_KIND_UINT8);
        component->SetInstanceSize(sizeof(uint8_t));

        std::memset(arrayStorage, 0, sizeof(arrayStorage));
        array = reinterpret_cast<TypeInfo*>(arrayStorage);
        array->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        array->SetComponentTypeInfo(component);

        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(this), sizeof(*this));
    }

    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)];
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)];
    TypeInfo* component = nullptr;
    TypeInfo* array = nullptr;
};

ByteArrayTypeInfos& GetByteArrayTypeInfos()
{
    static ByteArrayTypeInfos infos;
    return infos;
}

struct SegmentedArrayContext {
    static SegmentedArrayContext* current;

    explicit SegmentedArrayContext(MIndex arrayLength) : length(arrayLength)
    {
        previousMutator = ThreadLocal::GetMutator();
        mutator.Init();
        mutator.SetInSaferegion(Mutator::SAFE_REGION_FALSE);
        ThreadLocal::SetMutator(&mutator);

        LargeArrayInitTestHooks hooks;
        hooks.allocate = Allocate;
        hooks.onPublish = OnPublish;
        hooks.onYield = OnYield;
        hooks.onWithdraw = OnWithdraw;
        current = this;
        CJ_MRT_SetLargeArrayInitTestHooks(&hooks);
    }

    ~SegmentedArrayContext()
    {
        CJ_MRT_SetLargeArrayInitTestHooks(nullptr);
        current = nullptr;
        ThreadLocal::SetMutator(previousMutator);
        std::free(storage);
    }

    static MAddress Allocate(size_t size, AllocType)
    {
        void* memory = nullptr;
        if (posix_memalign(&memory, alignof(MArray), size) != 0) {
            return NULL_ADDRESS;
        }
        std::memset(memory, 0, size);
        current->storage = memory;
        current->storageSize = size;
        ++current->allocations;
        return reinterpret_cast<MAddress>(memory);
    }

    static void OnPublish(MArray* array)
    {
        ++current->publishCount;
        current->publishedArray = array;
    }

    static void OnYield(size_t segmentIndex)
    {
        ++current->yieldCount;
        current->lastYieldSegment = segmentIndex;
        if (current->yieldAction != nullptr) {
            current->yieldAction(segmentIndex);
        }
    }

    static void OnWithdraw(MArray* array)
    {
        ++current->withdrawCount;
        current->withdrawnArray = array;
    }

    using YieldAction = void (*)(size_t segmentIndex);
    MIndex length;
    Mutator mutator;
    Mutator* previousMutator = nullptr;
    void* storage = nullptr;
    size_t storageSize = 0;
    size_t allocations = 0;
    size_t publishCount = 0;
    size_t yieldCount = 0;
    size_t withdrawCount = 0;
    size_t lastYieldSegment = 0;
    MArray* publishedArray = nullptr;
    MArray* withdrawnArray = nullptr;
    YieldAction yieldAction = nullptr;

    size_t scannerRuns = 0;
    size_t scannerFailures = 0;
    bool flipped = false;
};

SegmentedArrayContext* SegmentedArrayContext::current = nullptr;

void ScanInvisibleRoot(size_t)
{
    SegmentedArrayContext& ctx = *SegmentedArrayContext::current;
    ++ctx.scannerRuns;
#if defined(__linux__)
    const pid_t child = fork();
    if (child == 0) {
#endif
        bool sawRoot = false;
        bool sawType = false;
        bool sawLength = false;
        size_t slots = 0;
        size_t nullSlots = 0;
        RootVisitor scanner = [&](RootSlot& root) {
            sawRoot = true;
            auto* array = static_cast<MArray*>(to_object(safe(root.LoadPlain(std::memory_order_acquire))));
            sawType = array->GetTypeInfo() == GetReferenceArrayTypeInfos().array;
            sawLength = array->GetLength() == ctx.length;
            array->ForEachRefField([&](RefField<>& slot) {
                ++slots;
                if (is_null(slot.GetFieldValue())) {
                    ++nullSlots;
                }
            });
        };
        ctx.mutator.VisitInvisibleRoot(scanner);
        const bool valid = sawRoot && sawType && sawLength && slots == static_cast<size_t>(ctx.length) &&
            nullSlots == slots;
#if defined(__linux__)
        _exit(valid ? 0 : 1);
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        ++ctx.scannerFailures;
    }
#else
    if (!valid) {
        ++ctx.scannerFailures;
    }
#endif
}

void FlipEpochAndRewriteFirstBlock(size_t segmentIndex)
{
    SegmentedArrayContext& ctx = *SegmentedArrayContext::current;
    if (ctx.flipped || segmentIndex != 0) {
        return;
    }
    ctx.flipped = true;
    g_gcCount.fetch_add(1, std::memory_order_release);
    std::memset(ctx.publishedArray->ConvertToCArray(), 0xa5,
                MArray::LARGE_REF_ARRAY_INIT_SEGMENT_SIZE);
}

bool AllSlotsAreRawNull(MArray* array)
{
    bool allNull = true;
    array->ForEachRefField([&](RefField<>& slot) {
        if (!is_null(slot.GetFieldValue())) {
            allNull = false;
        }
    });
    return allNull;
}
} // namespace

GC_TEST(SegmentedArrayInit, YieldKeepsInvisibleRootAndPublishesBoundary)
{
    GcHeapFixture heap;
    constexpr MIndex length = static_cast<MIndex>(
        (MArray::LARGE_REF_ARRAY_INIT_SEGMENT_SIZE * 2) / sizeof(void*) + 1);
    SegmentedArrayContext ctx(length);
    ctx.yieldAction = ScanInvisibleRoot;

    MArray* array = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, length);

    GC_EXPECT_TRUE(array != nullptr);
    GC_EXPECT_EQ(ctx.allocations, 1u);
    GC_EXPECT_EQ(ctx.publishCount, 1u);
    GC_EXPECT_TRUE(ctx.yieldCount >= 2u);
    GC_EXPECT_EQ(ctx.scannerRuns, ctx.yieldCount);
    GC_EXPECT_EQ(ctx.scannerFailures, 0u);
    GC_EXPECT_EQ(ctx.withdrawCount, 1u);
    GC_EXPECT_TRUE(ctx.withdrawnArray == array);
    GC_EXPECT_TRUE(ctx.mutator.LoadInvisibleRoot() == nullptr);
}

GC_TEST(SegmentedArrayInit, EpochFlipRestartsAndRewritesPublishedBlock)
{
    GcHeapFixture heap;
    constexpr MIndex length = static_cast<MIndex>(
        (MArray::LARGE_REF_ARRAY_INIT_SEGMENT_SIZE * 2) / sizeof(void*) + 1);
    SegmentedArrayContext ctx(length);
    ctx.yieldAction = FlipEpochAndRewriteFirstBlock;

    MArray* array = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, length);

    GC_EXPECT_TRUE(ctx.flipped);
    GC_EXPECT_TRUE(ctx.yieldCount >= 4u);
    GC_EXPECT_TRUE(AllSlotsAreRawNull(array));
    GC_EXPECT_EQ(ctx.withdrawCount, 1u);
    GC_EXPECT_TRUE(ctx.mutator.LoadInvisibleRoot() == nullptr);
}

GC_TEST(SegmentedArrayInit, SmallReferenceArrayKeepsFastPath)
{
    GcHeapFixture heap;
    constexpr MIndex length = 16;
    SegmentedArrayContext ctx(length);

    MArray* array = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, length);

    GC_EXPECT_TRUE(array != nullptr);
    GC_EXPECT_EQ(ctx.allocations, 1u);
    GC_EXPECT_EQ(ctx.publishCount, 0u);
    GC_EXPECT_EQ(ctx.yieldCount, 0u);
    GC_EXPECT_EQ(ctx.withdrawCount, 0u);
    GC_EXPECT_TRUE(AllSlotsAreRawNull(array));
}

GC_TEST(SegmentedArrayInit, LargePrimitiveArrayKeepsFastPath)
{
    GcHeapFixture heap;
    constexpr MIndex length = static_cast<MIndex>(MArray::LARGE_REF_ARRAY_INIT_SEGMENT_SIZE * 2 + 1);
    SegmentedArrayContext ctx(length);

    MArray* array = MCC_NewArray8(GetByteArrayTypeInfos().array, length);

    GC_EXPECT_TRUE(array != nullptr);
    GC_EXPECT_EQ(ctx.allocations, 1u);
    GC_EXPECT_EQ(ctx.publishCount, 0u);
    GC_EXPECT_EQ(ctx.yieldCount, 0u);
    GC_EXPECT_EQ(ctx.withdrawCount, 0u);
}

#endif // MRT_GC_UNIT_TESTS
