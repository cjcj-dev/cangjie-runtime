// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Collector/StringDedup.h"
#include <cstring>
#include "Heap/Heap.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
StringDedup& StringDedup::Instance()
{
    static StringDedup instance;
    return instance;
}

bool StringDedup::IsByteArray(BaseObject* object)
{
    return object != nullptr && Heap::IsHeapAddress(object) && object->IsRawArray() &&
        object->GetComponentTypeInfo()->GetType() == TypeKind::TYPE_KIND_UINT8;
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

void StringDedup::Request(BaseObject* object)
{
    // zStringDedup.inline.hpp:32: L01s substitutes UInt8 backing for String oop.
    if (!IsByteArray(object)) return;
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
    for (size_t i = 0, count = requests.size(); i < count; ++i) {
        WeakSlot slot = requests[i];
        BaseObject* object = Resolve(slot);
        if (object == nullptr || !isAlive(object)) slot.value = to_zpointer(0);
        requests[i] = slot;
    }
    for (auto it = table.begin(); it != table.end();) {
        BaseObject* object = Resolve(it->second);
        if (object == nullptr || !isAlive(object)) it = table.erase(it);
        else ++it;
    }
}

void StringDedup::Remap()
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for (size_t i = 0, count = requests.size(); i < count; ++i) {
        WeakSlot slot = requests[i];
        Resolve(slot);
        requests[i] = slot;
    }
    for (auto& entry : table) Resolve(entry.second);
}

size_t StringDedup::Hash(BaseObject* object)
{
    auto* array = static_cast<MArray*>(object);
    // StringDedupTable::compute_hash: hash bytes, confirm equality in the bucket.
    size_t hash = 0;
    for (MIndex i = 0; i < array->GetLength(); ++i) hash = hash * 31 + array->GetPrimitiveElement<U8>(i);
    return hash;
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
            // L01s: ordinary mutable byte arrays must never be redirected.
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
        condition.wait(guard, [this] { return stopped || (suspended == 0 && !requests.empty()); });
        if (stopped) break;
        // StringDedupProcessor::process_requests: release the request slot
        // before table lookup; the mutex keeps GC from clearing this local.
        WeakSlot slot = requests.back();
        requests.pop_back();
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
    if (data == nullptr || length == 0) return;
    auto* object = reinterpret_cast<MArray*>(reinterpret_cast<uintptr_t>(data) - MArray::GetContentOffset());
    if (!Heap::IsHeapAddress(object)) return;
    if (object->GetLength() != length) return;
    StringDedup::Instance().Request(object);
}
} // namespace MapleRuntime
