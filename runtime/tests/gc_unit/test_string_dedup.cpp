// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// Private table observations are available only in the matching product configuration.
#if defined(MRT_TESTABLE_INTERNALS)
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zStringDedup.hpp"
#include "ObjectModel/MArray.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {
extern "C" void CJ_MRT_RequestStringDedup(const uint8_t* data, size_t length);

struct StringDedupTestAccess {
    static uint32_t Hash(MArray* array, uint64_t seed)
    {
        auto& dedup = StringDedup::Instance();
        const auto saved = dedup.hashSeed;
        dedup.hashSeed = seed;
        const uint32_t hash = dedup.Hash(array);
        dedup.hashSeed = saved;
        return hash;
    }
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

// TestStringDeduplicationTableResize: equal-length distinct values stay separate.
GC_TEST(StringDedup, SameLengthKeepsDistinctBacking)
{
    ByteArrays arrays;
    arrays.first->SetPrimitiveElement<I8>(0, 0);
    arrays.first->SetPrimitiveElement<I8>(1, 31);
    arrays.second->SetPrimitiveElement<I8>(0, 1);
    arrays.second->SetPrimitiveElement<I8>(1, 0);
    CJ_MRT_RequestStringDedup(arrays.first->ConvertToCArray(), arrays.first->GetLength());
    CJ_MRT_RequestStringDedup(arrays.second->ConvertToCArray(), arrays.second->GetLength());
    GC_EXPECT_EQ(StringDedupTestAccess::Pending(), 2U);
    StringDedupTestAccess::ProcessRequests();
    GC_EXPECT_EQ(StringDedupTestAccess::Entries(), 2U);
}

// TestStringDeduplication: equal immutable String values share a weak table entry.
// The managed canonical return is tracked separately; this checks registration.
GC_TEST(StringDedup, ExplicitEqualStringBackingFindsEntry)
{
    ByteArrays arrays;
    for (auto* array : {arrays.first, arrays.second}) {
        array->SetPrimitiveElement<I8>(0, 7);
        array->SetPrimitiveElement<I8>(1, 9);
        CJ_MRT_RequestStringDedup(array->ConvertToCArray(), array->GetLength());
    }
    GC_EXPECT_EQ(StringDedupTestAccess::Pending(), 2U);
    StringDedupTestAccess::ProcessRequests();
    GC_EXPECT_EQ(StringDedupTestAccess::Entries(), 1U);
}

// TestStringDeduplicationFullGC: weak storage drops dead table and queued values.
// This component test supplies liveness; it does not assert a full GC was run.
GC_TEST(StringDedup, CleanDeadTableAndRequest)
{
    ByteArrays arrays;
    auto& dedup = StringDedup::Instance();
    arrays.first->SetPrimitiveElement<I8>(0, 7);
    arrays.first->SetPrimitiveElement<I8>(1, 9);
    CJ_MRT_RequestStringDedup(arrays.first->ConvertToCArray(), arrays.first->GetLength());
    StringDedupTestAccess::ProcessRequests();
    GC_EXPECT_EQ(StringDedupTestAccess::Entries(), 1U);
    CJ_MRT_RequestStringDedup(arrays.second->ConvertToCArray(), arrays.second->GetLength());
    dedup.Clean([&](BaseObject* object) { return object == arrays.first; });
    StringDedupTestAccess::ProcessRequests();
    GC_EXPECT_EQ(StringDedupTestAccess::Entries(), 1U);
    dedup.Clean([](BaseObject*) { return false; });
    GC_EXPECT_EQ(StringDedupTestAccess::Entries(), 0U);
}

// TestStringDeduplicationYoungGC adaptation: only explicit String ABI requests
// are supported; malformed backing is rejected before byte consumption.
GC_TEST(StringDedup, RejectOrdinaryObject)
{
    GcHeapFixture heap;
    auto& dedup = StringDedup::Instance();
    dedup.Stop();
    CJ_MRT_RequestStringDedup(reinterpret_cast<uint8_t*>(heap.obj0) + MArray::GetContentOffset(), 2);
    GC_EXPECT_EQ(StringDedupTestAccess::Pending(), 0U);
    dedup.Stop();
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
        const uint32_t hash = StringDedupTestAccess::Hash(arrays.first, 256 - length);
        for (unsigned byte = 0; byte != 4; ++byte) hashes[length * 4 + byte] = hash >> (byte * 8);
    }
    std::memcpy(bytes, hashes, sizeof(hashes));
    arrays.first->SetLength(sizeof(hashes));
    GC_EXPECT_EQ(StringDedupTestAccess::Hash(arrays.first, 0), 0xd2be7fd8U);
}

#endif // MRT_TESTABLE_INTERNALS
