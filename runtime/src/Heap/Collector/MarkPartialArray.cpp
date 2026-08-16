// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/MarkPartialArray.h"

#include <atomic>
#include <cstdlib>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace MarkPartialArray {
namespace {

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

std::atomic<uint64_t> g_arraysSplit{ 0 };
std::atomic<uint64_t> g_chunksPushed{ 0 };
std::atomic<uint64_t> g_chunksFollowed{ 0 };
std::atomic<uint64_t> g_notEncodable{ 0 };

} // namespace

bool Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_PARTIAL_ARRAY");
    return on;
}

bool Encodable(const void* chunkStart, size_t length)
{
    const MAddress addr = reinterpret_cast<MAddress>(chunkStart);
    const MAddress base = Heap::heapStartAddr;
    if (addr < base) {
        return false;
    }
    // push_partial_array asserts this in ZGC (zMark.cpp:186); the split in
    // FollowArrayElementsLarge is what makes it hold.
    if ((addr & (MIN_SIZE - 1)) != 0) {
        return false;
    }
    if (length == 0 || length > MAX_LENGTH) {
        return false;
    }
    return ((addr - base) >> MIN_SIZE_SHIFT) <= MAX_OFFSET;
}

BaseObject* Encode(const void* chunkStart, size_t length)
{
    const MAddress addr = reinterpret_cast<MAddress>(chunkStart);
    const uintptr_t offset = (addr - Heap::heapStartAddr) >> MIN_SIZE_SHIFT;
    const uintptr_t entry = (offset << OFFSET_SHIFT) |
                            (static_cast<uintptr_t>(length) << LENGTH_SHIFT) | TAG_MASK;
    return reinterpret_cast<BaseObject*>(entry);
}

void Decode(const BaseObject* entry, MAddress& chunkStart, size_t& length)
{
    const uintptr_t raw = reinterpret_cast<uintptr_t>(entry);
    const uintptr_t offset = raw >> OFFSET_SHIFT;
    length = static_cast<size_t>((raw >> LENGTH_SHIFT) & MAX_LENGTH);
    chunkStart = Heap::heapStartAddr + (offset << MIN_SIZE_SHIFT);
}

void NoteArraySplit() { (void)g_arraysSplit.fetch_add(1, std::memory_order_relaxed); }
void NoteChunkPushed() { (void)g_chunksPushed.fetch_add(1, std::memory_order_relaxed); }
void NoteChunkFollowed() { (void)g_chunksFollowed.fetch_add(1, std::memory_order_relaxed); }
void NoteNotEncodable() { (void)g_notEncodable.fetch_add(1, std::memory_order_relaxed); }

void Report(const char* point)
{
    if (!Enabled()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][partial-array] point=%s arrays_split=%zu chunks_pushed=%zu chunks_followed=%zu "
        "not_encodable=%zu min_length=%zu env=MRT_GCV2_PARTIAL_ARRAY=1",
        point, static_cast<size_t>(g_arraysSplit.load(std::memory_order_relaxed)),
        static_cast<size_t>(g_chunksPushed.load(std::memory_order_relaxed)),
        static_cast<size_t>(g_chunksFollowed.load(std::memory_order_relaxed)),
        static_cast<size_t>(g_notEncodable.load(std::memory_order_relaxed)),
        MIN_LENGTH);
}

} // namespace MarkPartialArray
} // namespace MapleRuntime
