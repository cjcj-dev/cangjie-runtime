// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// Private table observations are available only in the matching product configuration.
#if defined(MRT_TESTABLE_INTERNALS)
#include <cstdio>
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include "ObjectModel/MArray.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {
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
    static size_t Entries() { return StringDedup::Instance().table.size(); }
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

// TestStringDeduplicationFullGC: weak storage drops dead table and queued values.
// This component test supplies liveness; it does not assert a full GC was run.
GC_TEST(StringDedup, CleanDeadTableAndRequest)
{
    ByteArrays arrays;
    auto& dedup = StringDedup::Instance();
    arrays.first->SetPrimitiveElement<I8>(0, 7);
    arrays.first->SetPrimitiveElement<I8>(1, 9);
    auto* installed = MCC_StringDedupCanonicalImpl(arrays.heap.typeInfo, arrays.first);
    GC_EXPECT_TRUE(installed == arrays.first);
    GC_EXPECT_EQ(StringDedupTest::Entries(), 1U);
    arrays.second->SetPrimitiveElement<I8>(0, 1);
    arrays.second->SetPrimitiveElement<I8>(1, 2);
    auto* other = MCC_StringDedupCanonicalImpl(arrays.heap.typeInfo, arrays.second);
    GC_EXPECT_TRUE(other == arrays.second);
    GC_EXPECT_EQ(StringDedupTest::Entries(), 2U);
    dedup.Clean([&](BaseObject* object) { return object == arrays.first; });
    GC_EXPECT_EQ(StringDedupTest::Entries(), 1U);
    dedup.Clean([](BaseObject*) { return false; });
    GC_EXPECT_EQ(StringDedupTest::Entries(), 0U);
    auto* again = MCC_StringDedupCanonicalImpl(arrays.heap.typeInfo, arrays.first);
    GC_EXPECT_TRUE(again == arrays.first);
    GC_EXPECT_EQ(StringDedupTest::Entries(), 1U);
}

// TestStringDeduplicationYoungGC adaptation: only explicit String ABI requests
// are supported; malformed backing is rejected before byte consumption.
GC_TEST(StringDedup, RejectOrdinaryObject)
{
    GcHeapFixture heap;
    auto& dedup = StringDedup::Instance();
    dedup.Stop();
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

#endif // MRT_TESTABLE_INTERNALS
