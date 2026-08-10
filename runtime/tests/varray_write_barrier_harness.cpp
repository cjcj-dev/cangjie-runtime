// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Positive control for WRITE_SIDE_CLOSURE_PLAN G-C1/G-C2/G-C3:
// ref-bearing VArray direct-copy must post-record via WriteStruct/CopyStructArray;
// primitive VArray must stay on the no-record fast path.

#include <cstdlib>
#include <cstring>
#include <functional>
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
#include "ObjectModel/Flags.h"
#include "ObjectModel/MClass.h"
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

        // VArray<class-with-ref, 1> TypeInfo: component is class with FLAG_HAS_REF_FIELD.
        std::memset(varrayTiStorage, 0, sizeof(varrayTiStorage));
        varrayTi = reinterpret_cast<TypeInfo*>(varrayTiStorage);
        varrayTi->SetType(TypeKind::TYPE_KIND_VARRAY);
        varrayTi->SetComponentTypeInfo(typeInfo);
        varrayTi->SetFieldNum(1);
        varrayTi->SetInstanceSize(sizeof(RefField<>));

        // VArray<Int64, 1> TypeInfo: primitive component, no ref flag.
        std::memset(primCompStorage, 0, sizeof(primCompStorage));
        primComponent = reinterpret_cast<TypeInfo*>(primCompStorage);
        primComponent->SetType(TypeKind::TYPE_KIND_INT64);
        primComponent->SetInstanceSize(sizeof(int64_t));

        std::memset(primVarrayTiStorage, 0, sizeof(primVarrayTiStorage));
        primVarrayTi = reinterpret_cast<TypeInfo*>(primVarrayTiStorage);
        primVarrayTi->SetType(TypeKind::TYPE_KIND_VARRAY);
        primVarrayTi->SetComponentTypeInfo(primComponent);
        primVarrayTi->SetFieldNum(1);
        primVarrayTi->SetInstanceSize(sizeof(int64_t));

        youngRegion->SetYoungRegionFlag(1);
        youngRegion->SetYoungAge(1);
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
    alignas(TypeInfo) unsigned char varrayTiStorage[sizeof(TypeInfo)];
    TypeInfo* varrayTi = nullptr;
    alignas(TypeInfo) unsigned char primCompStorage[sizeof(TypeInfo)];
    TypeInfo* primComponent = nullptr;
    alignas(TypeInfo) unsigned char primVarrayTiStorage[sizeof(TypeInfo)];
    TypeInfo* primVarrayTi = nullptr;
};

size_t DrainCount(RememberedSet& rs)
{
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    return records.size();
}

// Mirror of the G-C1 branch after the fix (CompilerCalls.cpp TYPE_KIND_VARRAY).
void G_C1_ArrayCopyGenericBranch(Barrier& barrier, TypeInfo* componentTi, BaseObject* dstObj, MAddress dstField,
                                 size_t dstSize, BaseObject* srcObj, MAddress srcField, size_t srcSize)
{
    if (componentTi->HasRefField()) {
        barrier.CopyStructArray(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
    } else {
        (void)memmove_s(reinterpret_cast<void*>(dstField), dstSize, reinterpret_cast<void*>(srcField), srcSize);
    }
}

// Mirror of the G-C2 branch after the fix (FieldInfo SetValue / SetVArrayField).
void G_C2_SetVArrayFieldBranch(Barrier& barrier, TypeInfo* varrayTi, BaseObject* obj, MAddress fieldAddr,
                               MAddress src, size_t size)
{
    if (varrayTi->HasRefField()) {
        barrier.WriteStruct(obj, fieldAddr, size, src, size);
    } else {
        (void)memcpy_s(reinterpret_cast<void*>(fieldAddr), size, reinterpret_cast<void*>(src), size);
    }
}

// Mirror of the G-C3 branch after the fix (VArrayToAny / RetValueToAny / GetValue box).
void G_C3_VArrayBoxBranch(Barrier& barrier, TypeInfo* varrayTi, BaseObject* boxObj, MAddress dst, MAddress src,
                          size_t size)
{
    if (varrayTi->HasRefField()) {
        barrier.WriteStruct(boxObj, dst, size, src, size);
    } else {
        (void)memcpy_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size);
    }
}

// Pre-fix behaviour: unconditional memmove (must leave remset empty).
void PreFixBareCopy(MAddress dst, size_t dstSize, MAddress src, size_t srcSize)
{
    (void)memmove_s(reinterpret_cast<void*>(dst), dstSize, reinterpret_cast<void*>(src), srcSize);
}

bool RunArm(const char* arm, RememberedSet& rs, const std::function<void()>& body, size_t expectRecords)
{
    size_t before = DrainCount(rs);
    body();
    size_t after = DrainCount(rs);
    bool ok = before == 0 && after == expectRecords;
    std::cout << "POSCTRL arm=" << arm << " before=" << before << " after=" << after
              << " expect=" << expectRecords << " result=" << (ok ? "PASS" : "FAIL") << '\n';
    return ok;
}

} // namespace
} // namespace MapleRuntime

int main()
{
    using namespace MapleRuntime;
    RegionFixture fixture;
    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fixture.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    bool hasRef = fixture.varrayTi->HasRefField();
    bool primNoRef = !fixture.primVarrayTi->HasRefField();
    std::cout << "HAS_REF_FIELD ref_varray=" << hasRef << " prim_varray=" << (!primNoRef)
              << " result=" << ((hasRef && primNoRef) ? "PASS" : "FAIL") << '\n';
    bool passed = hasRef && primNoRef;

    RefField<> sourceField(to_zpointer(reinterpret_cast<MAddress>(fixture.youngObject)));
    MAddress dst = reinterpret_cast<MAddress>(fixture.field);
    MAddress src = reinterpret_cast<MAddress>(&sourceField);
    size_t sz = sizeof(RefField<>);

    // --- G-C1 ---
    passed = RunArm("G-C1-pre-bare-memmove", rs,
                    [&] {
                        fixture.field->StoreColoured(zpointer::null);
                        PreFixBareCopy(dst, sz, src, sz);
                    },
                    0) &&
        passed;
    passed = RunArm("G-C1-post-CopyStructArray", rs,
                    [&] {
                        fixture.field->StoreColoured(zpointer::null);
                        G_C1_ArrayCopyGenericBranch(barrier, fixture.varrayTi, fixture.oldObject, dst, sz, nullptr,
                                                    src, sz);
                    },
                    1) &&
        passed;
    passed = RunArm("G-C1-post-prim-skip", rs,
                    [&] {
                        fixture.field->StoreColoured(zpointer::null);
                        G_C1_ArrayCopyGenericBranch(barrier, fixture.primVarrayTi, fixture.oldObject, dst, sz, nullptr,
                                                    src, sz);
                    },
                    0) &&
        passed;

    // --- G-C2 ---
    passed = RunArm("G-C2-pre-bare-memcpy", rs,
                    [&] {
                        fixture.field->StoreColoured(zpointer::null);
                        PreFixBareCopy(dst, sz, src, sz);
                    },
                    0) &&
        passed;
    passed = RunArm("G-C2-post-WriteStruct", rs,
                    [&] {
                        fixture.field->StoreColoured(zpointer::null);
                        G_C2_SetVArrayFieldBranch(barrier, fixture.varrayTi, fixture.oldObject, dst, src, sz);
                    },
                    1) &&
        passed;
    passed = RunArm("G-C2-post-prim-skip", rs,
                    [&] {
                        fixture.field->StoreColoured(zpointer::null);
                        G_C2_SetVArrayFieldBranch(barrier, fixture.primVarrayTi, fixture.oldObject, dst, src, sz);
                    },
                    0) &&
        passed;

    // --- G-C3 (box into old holder; RAW_POINTER may be non-young) ---
    passed = RunArm("G-C3-pre-bare-memcpy", rs,
                    [&] {
                        fixture.field->StoreColoured(zpointer::null);
                        PreFixBareCopy(dst, sz, src, sz);
                    },
                    0) &&
        passed;
    passed = RunArm("G-C3-post-WriteStruct", rs,
                    [&] {
                        fixture.field->StoreColoured(zpointer::null);
                        G_C3_VArrayBoxBranch(barrier, fixture.varrayTi, fixture.oldObject, dst, src, sz);
                    },
                    1) &&
        passed;
    passed = RunArm("G-C3-post-prim-skip", rs,
                    [&] {
                        fixture.field->StoreColoured(zpointer::null);
                        G_C3_VArrayBoxBranch(barrier, fixture.primVarrayTi, fixture.oldObject, dst, src, sz);
                    },
                    0) &&
        passed;

    std::cout << "VARRAY_WB_POSCTRL result=" << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
