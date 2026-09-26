// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include <cstring>
#include <random>
#include "Heap/z/zHeap.hpp"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
StringDedup::StringDedup()
{
    std::random_device random;
    hashSeed = (static_cast<uint64_t>(random()) << 32) | random();
}

StringDedup& StringDedup::Instance()
{
    static StringDedup instance;
    return instance;
}

void StringDedup::Start()
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    stopped = false;
}

void StringDedup::Stop()
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    stopped = true;
    table.clear();
}

bool StringDedup::Accepts(const TypeInfo* arrayInfo, ArrayRef candidate)
{
    // zStringDedup.inline.hpp:38 requires String identity, not byte-array type.
    // The explicit ABI is that identity: only a full UInt8 RawArray is installed.
    if (candidate == nullptr || arrayInfo == nullptr || !Heap::IsHeapAddress(candidate)) {
        return false;
    }
    if (candidate->GetTypeInfo() != arrayInfo || !candidate->IsRawArray()) {
        return false;
    }
    TypeInfo* component = candidate->GetComponentTypeInfo();
    if (component == nullptr || component->GetType() != TypeKind::TYPE_KIND_UINT8) {
        return false;
    }
    return candidate->GetLength() != 0;
}

BaseObject* StringDedup::Resolve(WeakSlot& slot)
{
    RefField<> reference(slot.value);
    BaseObject* object = Heap::GetHeap().make_load_good(reference);
    slot.value = ZAddress::store_good_or_null(from_object(object));
    return object;
}

void StringDedup::Clean(const std::function<bool(BaseObject*)>& isAlive)
{
    // zWeakRootsProcessor.cpp: weak storage is cleared before reclaim and
    // resurrection unblock. Resolve does not mark or pin the backing.
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for (auto it = table.begin(); it != table.end();) {
        BaseObject* object = Resolve(it->second);
        if (object == nullptr || !isAlive(object)) it = table.erase(it);
        else ++it;
    }
    // StringDedupTable::Cleaner/Resizer release unused bucket capacity.
    table.rehash(0);
}

size_t StringDedup::Hash(BaseObject* object) const
{
    // StringDedupTable::compute_hash / AltHashing::halfsiphash_32: HalfSipHash-2-4.
    auto* array = static_cast<MArray*>(object);
    const uint8_t* bytes = array->ConvertToCArray();
    const size_t length = array->GetLength();
    uint32_t a = static_cast<uint32_t>(hashSeed);
    uint32_t b = static_cast<uint32_t>(hashSeed >> 32);
    uint32_t c = a ^ 0x6c796765U;
    uint32_t d = b ^ 0x74656462U;
    auto rotate = [](uint32_t value, unsigned count) {
        return (value << count) | (value >> (32 - count));
    };
    auto rounds = [&](unsigned count) {
        while (count-- != 0) {
            a += b; b = rotate(b, 5) ^ a; a = rotate(a, 16);
            c += d; d = rotate(d, 8) ^ c;
            a += d; d = rotate(d, 7) ^ a;
            c += b; b = rotate(b, 13) ^ c; c = rotate(c, 16);
        }
    };
    auto absorb = [&](uint32_t word) { d ^= word; rounds(2); a ^= word; };
    size_t offset = 0;
    while (length - offset >= 4) {
        uint32_t word = 0;
        for (unsigned byte = 0; byte != 4; ++byte) word |= uint32_t(bytes[offset++]) << (8 * byte);
        absorb(word);
    }
    uint32_t tail = static_cast<uint32_t>(length) << 24;
    for (unsigned byte = 0; offset != length; ++byte) tail |= uint32_t(bytes[offset++]) << (8 * byte);
    absorb(tail);
    c ^= 0xffU;
    rounds(4);
    return b ^ d;
}

ArrayRef StringDedup::Canonical(const TypeInfo* arrayInfo, ArrayRef candidate)
{
    if (!Accepts(arrayInfo, candidate)) {
        return candidate;
    }
    std::lock_guard<std::recursive_mutex> guard(mutex);
    const size_t hash = Hash(candidate);
    const auto range = table.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
        auto* known = static_cast<MArray*>(Resolve(it->second));
        if (known == nullptr || known->GetLength() != candidate->GetLength()) {
            continue;
        }
        if (known == candidate ||
            std::memcmp(known->ConvertToCArray(), candidate->ConvertToCArray(), candidate->GetLength()) == 0) {
            // stringDedupTable.cpp:634: found != value => use the table array.
            if (known != candidate) {
                return known;
            }
            return candidate;
        }
    }
    table.emplace(hash, WeakSlot{ZAddress::store_good(from_object(candidate))});
    return candidate;
}
} // namespace MapleRuntime
