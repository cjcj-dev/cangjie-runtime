// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Collector/StringDedup.h"
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
    if (!stopped) return;
    stopped = false;
    processor = std::thread(&StringDedup::Run, this);
}

void StringDedup::Stop()
{
    {
        std::lock_guard<std::recursive_mutex> guard(mutex);
        stopped = true;
        condition.notify_all();
    }
    if (processor.joinable()) processor.join();
    std::lock_guard<std::recursive_mutex> guard(mutex);
    requests.clear();
    processing.clear();
    table.clear();
}

StringDedup::GCScope::GCScope()
{
    auto& dedup = Instance();
    std::lock_guard<std::recursive_mutex> guard(dedup.mutex);
    ++dedup.suspended;
}

StringDedup::GCScope::~GCScope()
{
    auto& dedup = Instance();
    std::lock_guard<std::recursive_mutex> guard(dedup.mutex);
    --dedup.suspended;
    dedup.condition.notify_all();
}

void StringDedup::RequestString(const uint8_t* data, size_t length)
{
    // zStringDedup.inline.hpp:38 requires String identity, not byte-array type.
    // String is a value type here, so only its explicit pinned-input ABI can
    // establish that contract. GC promotion cannot identify String backing.
    if (data == nullptr || length == 0) return;
    auto* object = reinterpret_cast<MArray*>(reinterpret_cast<uintptr_t>(data) - MArray::GetContentOffset());
    if (!Heap::IsHeapAddress(object) || !object->IsRawArray() ||
        object->GetComponentTypeInfo()->GetType() != TypeKind::TYPE_KIND_UINT8 ||
        object->GetLength() != length) return;
    std::lock_guard<std::recursive_mutex> guard(mutex);
    requests.push_back({to_zpointer(reinterpret_cast<uintptr_t>(object) | ::g_cjStoreGoodMask)});
    condition.notify_all();
}

BaseObject* StringDedup::Resolve(WeakSlot& slot)
{
    RefField<> reference(slot.value);
    BaseObject* object = Heap::GetHeap().GetCollector().make_load_good(reference, {});
    slot.value = to_zpointer(object == nullptr ? 0 :
        reinterpret_cast<uintptr_t>(object) | ::g_cjStoreGoodMask);
    return object;
}

void StringDedup::Clean(const std::function<bool(BaseObject*)>& isAlive)
{
    // zWeakRootsProcessor.cpp: weak storage is cleared before reclaim and
    // resurrection unblock. Resolve does not mark or pin the backing.
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for (auto* storage : {&requests, &processing}) {
        for (size_t i = 0, count = storage->size(); i < count; ++i) {
            WeakSlot slot = (*storage)[i];
            BaseObject* object = Resolve(slot);
            if (object == nullptr || !isAlive(object)) slot.value = to_zpointer(0);
            (*storage)[i] = slot;
        }
    }
    for (auto it = table.begin(); it != table.end();) {
        BaseObject* object = Resolve(it->second);
        if (object == nullptr || !isAlive(object)) it = table.erase(it);
        else ++it;
    }
    // StringDedupTable::Cleaner/Resizer release unused bucket capacity.
    table.rehash(0);
}

void StringDedup::Remap()
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for (auto* storage : {&requests, &processing}) {
        for (size_t i = 0, count = storage->size(); i < count; ++i) {
            WeakSlot slot = (*storage)[i];
            Resolve(slot);
            (*storage)[i] = slot;
        }
    }
    for (auto& entry : table) Resolve(entry.second);
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

void StringDedup::Process(WeakSlot slot)
{
    BaseObject* object = Resolve(slot);
    if (object == nullptr) return; // request was cleared by GC
    auto* candidate = static_cast<MArray*>(object);
    const size_t hash = Hash(object);
    const auto range = table.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
        auto* known = static_cast<MArray*>(Resolve(it->second));
        if (known != nullptr && known->GetLength() == candidate->GetLength() &&
            std::memcmp(known->ConvertToCArray(), candidate->ConvertToCArray(), candidate->GetLength()) == 0) {
            // L01s: this String backing is known; registration is complete.
            // Returning canonical managed backing needs a compiler intrinsic.
            return;
        }
    }
    table.emplace(hash, slot);
}

void StringDedup::Run()
{
    std::unique_lock<std::recursive_mutex> guard(mutex);
    while (!stopped) {
        condition.wait(guard, [this] { return stopped || (suspended == 0 && (!requests.empty() || !processing.empty())); });
        if (stopped) break;
        // StringDedupProcessor::process_requests: release the request slot
        // before table lookup; the mutex keeps GC from clearing this local.
        // Swap producer/consumer weak storages after all producers release
        // the mutex (StringDedupProcessor::wait_for_requests).
        if (processing.empty()) processing.swap(requests);
        WeakSlot slot = processing.back();
        processing.pop_back();
        Process(slot);
        guard.unlock();
        std::this_thread::yield();
        guard.lock();
    }
}

// Existing acquireRawData/releaseRawData ABI pins only for this call. The
// stored reference is weak; it never retains that pin beyond registration.
extern "C" MRT_EXPORT void CJ_MRT_RequestStringDedup(const uint8_t* data, size_t length)
{
    StringDedup::Instance().RequestString(data, length);
}
} // namespace MapleRuntime
