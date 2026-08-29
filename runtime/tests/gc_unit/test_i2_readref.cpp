// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC's load barrier resolves a load-bad from-address before self-healing, while
// a load-good word naming a from-copy is an assertion state
// (zBarrier.inline.hpp:294-343, zAddress.inline.hpp:609-624).

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include "Common/ColourPredicates.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Collector/Collector.h"
#include "Heap/WCollector/EnumBarrier.h"
#include "ObjectModel/RefField.inline.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

class ToCollector final : public Collector {
public:
    BaseObject* from = nullptr;
    BaseObject* to = nullptr;
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    FindToVersionResult FindToVersion(BaseObject* obj) const override
    {
        return obj == from ? FindToVersionResult::Found(to) : FindToVersionResult::NotForwarded();
    }
    BaseObject* ResolveStoreValue(BaseObject* obj) const override
    {
        return obj == from ? to : obj;
    }
    ZGenerationId remap_generation(RefField<>&) const override { return ZGenerationId::old; }
    BaseObject* relocate_or_remap_object(BaseObject* obj, ZGenerationId) const override
    {
        return ResolveStoreValue(obj);
    }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    bool IsGhostFromObject(BaseObject*) const override { return false; }
    bool IsUnmovableFromObject(BaseObject*) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        const uintptr_t remap = ColourPredicates::current_remapped(static_cast<uintptr_t>(::g_cjLoadBadMask));
        return RefField<>(GcUnit::ColouredPointer(obj, remap));
    }
};

} // namespace

GC_TEST(I2ReadRef, LoadBadForwardedFromResolvesAndHealsTo)
{
    GcHeapFixture fx;
    ToCollector collector;
    collector.from = fx.obj0;
    collector.to = fx.obj1;
    fx.obj0->SetStateCode(ObjectState::FORWARDED);

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    EnumBarrier barrier(collector, rs);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const uintptr_t staleRemaps = static_cast<uintptr_t>(::g_cjLoadBadMask) & REMAP_COLOUR_MASK;
    const uintptr_t remap = staleRemaps & (~staleRemaps + 1);
    GC_EXPECT_TRUE(remap != 0);
    field->StoreColoured(GcUnit::ColouredPointer(fx.obj0, remap));

    BaseObject* got = barrier.ReadReference(fx.obj0, *field);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.obj1));
    GC_EXPECT_TRUE(got != fx.obj0);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(to_object(field->GetTargetObject())),
                 reinterpret_cast<uintptr_t>(fx.obj1));
}

GC_TEST(I2ReadRef, ForgedLoadGoodForwardedFromIsRejected)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        GcHeapFixture fx;
        ToCollector collector;
        collector.from = fx.obj0;
        collector.to = fx.obj1;
        fx.obj0->SetStateCode(ObjectState::FORWARDED);

        RememberedSet rs;
        rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
        EnumBarrier barrier(collector, rs);
        auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
        const uintptr_t remap =
            ColourPredicates::current_remapped(static_cast<uintptr_t>(::g_cjLoadBadMask));
        field->StoreColoured(to_zpointer(reinterpret_cast<MAddress>(fx.obj0) | remap));
        (void)barrier.ReadReference(fx.obj0, *field);
        _exit(0);
    }

    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
}

GC_TEST(I2ReadRef, PlainHeapSlotIsHealedToCurrentColour)
{
    GcHeapFixture fx;
    ToCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    EnumBarrier barrier(collector, rs);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    const uintptr_t plain = reinterpret_cast<uintptr_t>(fx.obj0);
    std::memcpy(field, &plain, sizeof(plain));

    BaseObject* got = barrier.ReadReference(fx.obj1, *field);
    GC_EXPECT_TRUE(got == fx.obj0);
    GC_EXPECT_TRUE(ClassifySlotWord(static_cast<uintptr_t>(raw(field->GetFieldValue()))) ==
                   SlotWordVerdict::kColoured);
    GC_EXPECT_TRUE(static_cast<uintptr_t>(raw(field->GetFieldValue())) != plain);
}
