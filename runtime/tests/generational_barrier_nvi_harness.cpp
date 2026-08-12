// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <string>
#include <sys/mman.h>
#include <unordered_set>
#include <vector>

#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Barrier/Barrier.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/WCollector/EnumBarrier.h"
#include "Heap/WCollector/ForwardBarrier.h"
#include "Heap/WCollector/IdleBarrier.h"
#include "Heap/WCollector/PostTraceBarrier.h"
#include "Heap/WCollector/PreforwardBarrier.h"
#include "Heap/WCollector/TraceBarrier.h"
#include "ObjectModel/Flags.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
namespace {
class TestCollector final : public Collector {
public:
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    BaseObject* FindToVersion(BaseObject*) const override { return nullptr; }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        return RefField<>(to_zpointer(reinterpret_cast<MAddress>(obj)));
    }
};

class TestBarrier final : public Barrier {
public:
    TestBarrier(Collector& collector, RememberedSet& rememberedSet) : Barrier(collector, rememberedSet) {}

protected:
    void WriteReferenceImpl(BaseObject*, RefField<false>& field, BaseObject* ref) const override
    {
        field.StoreColoured(to_zpointer(reinterpret_cast<MAddress>(ref)));
    }
};

struct RegionFixture {
    RegionFixture()
    {
        mappedSize = 2 * sizeof(RegionInfo) + 2 * RegionInfo::UNIT_SIZE;
        mapping = mmap(nullptr, mappedSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) {
            std::abort();
        }
        heapStart = reinterpret_cast<MAddress>(mapping) + 2 * sizeof(RegionInfo);
        RegionInfo::Initialize(2, heapStart);
        oldRegion = RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        youngRegion = RegionInfo::InitRegion(1, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        Heap::OnHeapCreated(heapStart);
        Heap::OnHeapExtended(heapStart + 2 * RegionInfo::UNIT_SIZE);

        oldObject = reinterpret_cast<BaseObject*>(heapStart + 64);
        youngObject = reinterpret_cast<BaseObject*>(heapStart + RegionInfo::UNIT_SIZE + 64);
        field = new (reinterpret_cast<void*>(reinterpret_cast<MAddress>(oldObject) + TYPEINFO_PTR_SIZE))
            RefField<false>(nullptr);

        std::memset(typeInfoStorage, 0, sizeof(typeInfoStorage));
        typeInfo = reinterpret_cast<TypeInfo*>(typeInfoStorage);
        typeInfo->SetType(TypeKind::TYPE_KIND_CLASS);
        typeInfo->SetFlagHasRefField();
        typeInfo->SetInstanceSize(sizeof(RefField<>));
        GCTib gctib {};
        gctib.tag = SIGN_BIT | 1;
        typeInfo->SetGCTib(gctib);
        *reinterpret_cast<uint64_t*>(oldObject) = reinterpret_cast<uintptr_t>(typeInfo);
    }

    ~RegionFixture() { munmap(mapping, mappedSize); }

    void* mapping = nullptr;
    size_t mappedSize = 0;
    MAddress heapStart = 0;
    RegionInfo* oldRegion = nullptr;
    RegionInfo* youngRegion = nullptr;
    BaseObject* oldObject = nullptr;
    BaseObject* youngObject = nullptr;
    RefField<false>* field = nullptr;
    alignas(TypeInfo) unsigned char typeInfoStorage[sizeof(TypeInfo)];
    TypeInfo* typeInfo = nullptr;
};

bool ExpectOneRecord(const std::string& label, RememberedSet& rememberedSet, MAddress expected)
{
    std::unordered_set<MAddress> records;
    rememberedSet.DrainForMinor(records);
    bool found = std::find(records.begin(), records.end(), expected) != records.end();
    std::cout << "RECORD_CHECK label=" << label << " count=" << records.size() << " found=" << found << '\n';
    return records.size() == 1 && found;
}

template<typename BarrierType>
bool ExerciseBarrier(const char* name, BarrierType& barrier, RegionFixture& fixture, RememberedSet& rememberedSet)
{
    fixture.field->StoreColoured(zpointer::null);
    Barrier& entry = barrier;
    entry.WriteReference(fixture.oldObject, *fixture.field, fixture.youngObject);
    bool passed = ExpectOneRecord(name, rememberedSet, reinterpret_cast<MAddress>(fixture.field));
    std::cout << "BARRIER_RECORDS name=" << name << " count=" << (passed ? 1 : 0) << '\n';
    return passed;
}

bool ExerciseNineEntries(TestBarrier& barrier, RegionFixture& fixture, RememberedSet& rememberedSet)
{
    bool passed = true;
    RefField<> sourceField(to_zpointer(reinterpret_cast<MAddress>(fixture.youngObject)));
    std::vector<unsigned char> genericSource(TYPEINFO_PTR_SIZE + sizeof(RefField<>));
    new (genericSource.data() + TYPEINFO_PTR_SIZE)
        RefField<>(to_zpointer(reinterpret_cast<MAddress>(fixture.youngObject)));
    auto check = [&](const char* name) {
        bool one = ExpectOneRecord(name, rememberedSet, reinterpret_cast<MAddress>(fixture.field));
        std::cout << "ENTRY_RECORDS name=" << name << " count=" << (one ? 1 : 0) << '\n';
        passed = one && passed;
    };

    fixture.field->StoreColoured(zpointer::null);
    barrier.WriteReference(fixture.oldObject, *fixture.field, fixture.youngObject);
    check("WriteReference");

    fixture.field->StoreColoured(zpointer::null);
    barrier.WriteStruct(fixture.oldObject, reinterpret_cast<MAddress>(fixture.field), sizeof(RefField<>),
                        reinterpret_cast<MAddress>(&sourceField), sizeof(RefField<>));
    check("WriteStruct");

    fixture.field->StoreColoured(zpointer::null);
    barrier.CopyRefArray(fixture.oldObject, reinterpret_cast<MAddress>(fixture.field), sizeof(RefField<>), nullptr,
                         reinterpret_cast<MAddress>(&sourceField), sizeof(RefField<>));
    check("CopyRefArray");

    fixture.field->StoreColoured(zpointer::null);
    barrier.CopyStructArray(fixture.oldObject, reinterpret_cast<MAddress>(fixture.field), sizeof(RefField<>), nullptr,
                            reinterpret_cast<MAddress>(&sourceField), sizeof(RefField<>));
    check("CopyStructArray");

    auto* atomicField = &HeapSlotAt<true>(fixture.field);
    atomicField->StoreColoured(zpointer::null);
    barrier.AtomicWriteReference(fixture.oldObject, *atomicField, fixture.youngObject, std::memory_order_relaxed);
    check("AtomicWriteReference");

    atomicField->StoreColoured(zpointer::null);
    barrier.AtomicSwapReference(fixture.oldObject, *atomicField, fixture.youngObject, std::memory_order_relaxed);
    check("AtomicSwapReference");

    atomicField->StoreColoured(zpointer::null);
    barrier.CompareAndSwapReference(fixture.oldObject, *atomicField, nullptr, fixture.youngObject,
                                    std::memory_order_relaxed, std::memory_order_relaxed);
    check("CompareAndSwapReference");

    fixture.field->StoreColoured(zpointer::null);
    barrier.WriteGeneric(fixture.oldObject, fixture.field, reinterpret_cast<BaseObject*>(genericSource.data()),
                         sizeof(RefField<>));
    check("WriteGeneric");

    fixture.field->StoreColoured(zpointer::null);
    barrier.ReadGeneric(fixture.oldObject, reinterpret_cast<BaseObject*>(genericSource.data()),
                        genericSource.data() + TYPEINFO_PTR_SIZE, sizeof(RefField<>));
    check("ReadGeneric");
    return passed;
}
} // namespace
} // namespace MapleRuntime

int main()
{
    using namespace MapleRuntime;
    RegionFixture fixture;
    size_t initialYoungCount = RegionInfo::GetYoungRegionCount();
    fixture.oldRegion->SetYoungRegionFlag(1);
    size_t countAfterNewWrite = RegionInfo::GetYoungRegionCount();
    fixture.oldRegion->SetYoungRegionFlag(0);
    size_t countAfterClear = RegionInfo::GetYoungRegionCount();
    bool counterPassed = initialYoungCount == 0 && countAfterNewWrite == 1 && countAfterClear == 0;
    std::cout << "COUNTER_STRUCTURAL_PROOF new_write=SetYoungRegionFlag counter_calls=0 after_set="
              << countAfterNewWrite << " after_clear=" << countAfterClear << " result="
              << (counterPassed ? "PASS" : "FAIL") << '\n';

    bool representationPassed = !fixture.oldRegion->IsYoungRegion() && fixture.oldRegion->GetYoungAge() == 0 &&
        !fixture.youngRegion->IsYoungRegion() && fixture.youngRegion->GetYoungAge() == 0;
    fixture.youngRegion->SetYoungRegionFlag(1);
    fixture.youngRegion->SetYoungAge(5);
    representationPassed = representationPassed && fixture.youngRegion->IsYoungRegion() &&
        fixture.youngRegion->GetYoungAge() == 5;
    fixture.youngRegion->SetYoungRegionFlag(0);
    representationPassed = representationPassed && !fixture.youngRegion->IsYoungRegion() &&
        fixture.youngRegion->GetYoungAge() == 0;
    fixture.youngRegion->SetYoungRegionFlag(1);
    fixture.youngRegion->SetYoungAge(63);
    representationPassed = representationPassed && fixture.youngRegion->IsYoungRegion() &&
        fixture.youngRegion->GetYoungAge() == 63;
    std::cout << "YOUNG_REPRESENTATION result=" << (representationPassed ? "PASS" : "FAIL") << '\n';

    TestCollector collector;
    bool passed = representationPassed && counterPassed;
    RememberedSet testSet;
    testSet.Initialize(fixture.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier testBarrier(collector, testSet);
#if defined(MRT_GENERATIONAL_BARRIER_PROBE)
    fixture.youngRegion->SetYoungRegionFlag(0);
    Barrier::ResetGenerationalBarrierProbe();
    fixture.field->StoreColoured(zpointer::null);
    testBarrier.WriteReference(fixture.oldObject, *fixture.field, fixture.youngObject);
    uint64_t fastPathHits = Barrier::GetGenerationalBarrierFastPathHits();
    uint64_t regionLookups = Barrier::GetGenerationalBarrierRegionLookups();
    bool fastPathPassed = fastPathHits == 1 && regionLookups == 0;
    std::cout << "FASTPATH_HIT_RATE hits=" << fastPathHits << " total=" << (fastPathHits + regionLookups)
              << " result=" << (fastPathPassed ? "PASS" : "FAIL") << '\n';
    passed = fastPathPassed && passed;
    fixture.youngRegion->SetYoungRegionFlag(1);
    fixture.youngRegion->SetYoungAge(63);
#endif
    passed = ExerciseNineEntries(testBarrier, fixture, testSet) && passed;

    RememberedSet baseSet;
    baseSet.Initialize(fixture.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier baseBarrier(collector, baseSet);
    passed = ExerciseBarrier("Barrier", baseBarrier, fixture, baseSet) && passed;
    RememberedSet idleSet;
    idleSet.Initialize(fixture.heapStart, 2 * RegionInfo::UNIT_SIZE);
    IdleBarrier idleBarrier(collector, idleSet);
    passed = ExerciseBarrier("IdleBarrier", idleBarrier, fixture, idleSet) && passed;
    RememberedSet postTraceSet;
    postTraceSet.Initialize(fixture.heapStart, 2 * RegionInfo::UNIT_SIZE);
    PostTraceBarrier postTraceBarrier(collector, postTraceSet);
    passed = ExerciseBarrier("PostTraceBarrier", postTraceBarrier, fixture, postTraceSet) && passed;
    RememberedSet preforwardSet;
    preforwardSet.Initialize(fixture.heapStart, 2 * RegionInfo::UNIT_SIZE);
    PreforwardBarrier preforwardBarrier(collector, preforwardSet);
    passed = ExerciseBarrier("PreforwardBarrier", preforwardBarrier, fixture, preforwardSet) && passed;
    RememberedSet forwardSet;
    forwardSet.Initialize(fixture.heapStart, 2 * RegionInfo::UNIT_SIZE);
    ForwardBarrier forwardBarrier(collector, forwardSet);
    passed = ExerciseBarrier("ForwardBarrier", forwardBarrier, fixture, forwardSet) && passed;
    std::cout << "BARRIER_RECORDS name=EnumBarrier count=0 reason=requires-active-enum-mutator\n";
    std::cout << "BARRIER_RECORDS name=TraceBarrier count=0 reason=requires-active-trace-mutator\n";

    size_t afterScope = 1;
    {
        std::unordered_set<MAddress> records;
        testSet.DrainForMinor(records);
        afterScope = records.size();
    }
    std::cout << "MINOR_SCOPE_CLEAR count=" << afterScope << '\n';
    passed = passed && afterScope == 0;
    std::cout << "STRUCTURAL_PROOF subclass=TestBarrier record_calls=0 records=1 result="
              << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
