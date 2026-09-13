// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/Collector/StringDedup.h"
#include "ObjectModel/MArray.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {
struct StringDedupTestAccess {
    static size_t Pending() { return StringDedup::Instance().requests.size(); }
    static size_t Entries() { return StringDedup::Instance().table.size(); }
    static void ProcessRequests()
    {
        auto& dedup = StringDedup::Instance();
        while (!dedup.requests.empty()) {
            auto slot = dedup.requests.back();
            dedup.requests.pop_back();
            dedup.Process(slot);
        }
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

// TestStringDeduplicationTableResize: hash equality does not establish byte equality.
GC_TEST(StringDedup, HashCollisionKeepsDistinctBacking)
{
    ByteArrays arrays;
    arrays.first->SetPrimitiveElement<U8>(0, 0);
    arrays.first->SetPrimitiveElement<U8>(1, 31);
    arrays.second->SetPrimitiveElement<U8>(0, 1);
    arrays.second->SetPrimitiveElement<U8>(1, 0);
    auto& dedup = StringDedup::Instance();
    dedup.Request(arrays.first);
    dedup.Request(arrays.second);
    GC_EXPECT_EQ(StringDedupTestAccess::Pending(), 2U);
    StringDedupTestAccess::ProcessRequests();
    GC_EXPECT_EQ(StringDedupTestAccess::Entries(), 2U);
}

// TestStringDeduplicationFullGC: weak storage drops dead table and queued values.
// This component test supplies liveness; it does not assert a full GC was run.
GC_TEST(StringDedup, CleanDeadTableAndRequest)
{
    ByteArrays arrays;
    auto& dedup = StringDedup::Instance();
    arrays.first->SetPrimitiveElement<U8>(0, 7);
    arrays.first->SetPrimitiveElement<U8>(1, 9);
    dedup.Request(arrays.first);
    StringDedupTestAccess::ProcessRequests();
    GC_EXPECT_EQ(StringDedupTestAccess::Entries(), 1U);
    dedup.Request(arrays.second);
    dedup.Clean([&](BaseObject* object) { return object == arrays.first; });
    StringDedupTestAccess::ProcessRequests();
    GC_EXPECT_EQ(StringDedupTestAccess::Entries(), 1U);
    dedup.Clean([](BaseObject*) { return false; });
    GC_EXPECT_EQ(StringDedupTestAccess::Entries(), 0U);
}

// TestStringDeduplicationYoungGC: candidate filtering is by backing type.
GC_TEST(StringDedup, RejectOrdinaryObject)
{
    GcHeapFixture heap;
    auto& dedup = StringDedup::Instance();
    dedup.Stop();
    dedup.Request(heap.obj0);
    GC_EXPECT_EQ(StringDedupTestAccess::Pending(), 0U);
    dedup.Stop();
}
