// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/MarkPartialArray.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Heap.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/RefField.inline.h"

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
    static const bool on = []() {
        const char* value = std::getenv("MRT_GCV2_PARTIAL_ARRAY");
        return value == nullptr || value[0] != '0' || value[1] != '\0';
    }();
    return on;
}

bool Encodable(const void* chunkStart, size_t length)
{
    const MAddress addr = reinterpret_cast<MAddress>(chunkStart);
    const MAddress base = Heap::GetHeapStartAddress();
    if (addr < base) {
        return false;
    }
    // Encode/Decode store (addr - base) >> MIN_SIZE_SHIFT and reconstruct
    // base + (offset << MIN_SIZE_SHIFT).  Keep this predicate relative to
    // that same base (zMark.cpp:177-186).
    if (((addr - base) & (MIN_SIZE - 1)) != 0) {
        return false;
    }
    if (length == 0 || length > MAX_LENGTH) {
        return false;
    }
    return ((addr - base) >> MIN_SIZE_SHIFT) <= MAX_OFFSET;
}

MarkStackEntry Encode(const void* chunkStart, size_t length, bool finalizable)
{
    const MAddress addr = reinterpret_cast<MAddress>(chunkStart);
    const size_t offset = static_cast<size_t>((addr - Heap::GetHeapStartAddress()) >> MIN_SIZE_SHIFT);
    return MarkStackEntry::PartialArray(offset, length, finalizable);
}

void Decode(const MarkStackEntry& entry, MAddress& chunkStart, size_t& length)
{
    const size_t offset = entry.partialArrayOffset();
    length = entry.partialArrayLength();
    chunkStart = Heap::GetHeapStartAddress() + (offset << MIN_SIZE_SHIFT);
}

// ZGC zMark.cpp:216-270. Always visit the leading range locally; only
// publish ranges whose complete descriptor can be represented. The inline
// fallback visits every field of a legal but unencodable array.
void FollowElements(MAddress start, size_t length, bool finalizable,
                    const FieldVisitor& visit, const EntryPublisher& publish)
{
    const MAddress end = start + length * sizeof(MAddress);
    const MAddress middleStart = AlignUp(start + sizeof(MAddress), MIN_SIZE);
    if (!Enabled() || length <= MIN_LENGTH || length > MAX_LENGTH ||
        !Encodable(reinterpret_cast<const void*>(AlignDown(end, MIN_SIZE)), 1)) {
        if (Enabled() && length > MIN_LENGTH) {
            NoteNotEncodable();
        }
        for (size_t i = 0; i < length; ++i) {
            visit(start + i * sizeof(MAddress));
        }
        return;
    }
    const size_t middleLength = AlignDown((end - middleStart) / sizeof(MAddress), MIN_LENGTH);
    const MAddress middleEnd = middleStart + middleLength * sizeof(MAddress);
    auto push = [&](MAddress address, size_t count) {
        if (!Encodable(reinterpret_cast<const void*>(address), count)) {
            NoteNotEncodable();
            for (size_t i = 0; i < count; ++i) {
                visit(address + i * sizeof(MAddress));
            }
            return;
        }
        NoteChunkPushed();
        publish(Encode(reinterpret_cast<const void*>(address), count, finalizable));
    };
    NoteArraySplit();
    if (end > middleEnd) {
        push(middleEnd, (end - middleEnd) / sizeof(MAddress));
    }
    MAddress part = middleEnd;
    while (part > middleStart) {
        const size_t count = AlignUp((part - middleStart) / sizeof(MAddress) / 2, MIN_LENGTH);
        part -= count * sizeof(MAddress);
        push(part, count);
    }
    for (MAddress field = start; field < middleStart; field += sizeof(MAddress)) {
        visit(field);
    }
}

void FollowObjectReferences(BaseObject* object, bool finalizable,
                            const FieldVisitor& visit, const EntryPublisher& publish)
{
    if (object->GetTypeInfo()->IsRawArray()) {
        MArray* array = reinterpret_cast<MArray*>(object);
        TypeInfo* component = array->GetComponentTypeInfo();
        if (component->IsObjectType() || component->IsArrayType() || component->IsInterface()) {
            FollowElements(reinterpret_cast<MAddress>(array->ConvertToCArray()), array->GetLength(), finalizable, visit, publish);
            return;
        }
    }
    object->ForEachRefField([&](RefField<>& field) { visit(reinterpret_cast<MAddress>(&field)); });
}

void FollowPartialReferences(const MarkStackEntry& entry,
                             const FieldVisitor& visit, const EntryPublisher& publish)
{
    MAddress start = 0;
    size_t length = 0;
    Decode(entry, start, length);
    NoteChunkFollowed();
    FollowElements(start, length, entry.finalizable(), visit, publish);
}

void NoteArraySplit() { (void)g_arraysSplit.fetch_add(1, std::memory_order_relaxed); }
void NoteChunkPushed() { (void)g_chunksPushed.fetch_add(1, std::memory_order_relaxed); }
void NoteChunkFollowed() { (void)g_chunksFollowed.fetch_add(1, std::memory_order_relaxed); }
void NoteNotEncodable() { (void)g_notEncodable.fetch_add(1, std::memory_order_relaxed); }

// MRT_GCV2_PARTIAL_ARRAY_REPORT=1 forces the counters out even when chunking
// is off, so the control arm can show an actual zero instead of an absent
// line -- "no output" would read the same whether the counters are zero or
// the report itself never ran.
bool ForceReport()
{
    static const bool on = EnvIsOne("MRT_GCV2_PARTIAL_ARRAY_REPORT");
    return on;
}

void Report(const char* point)
{
    if (!Enabled() && !ForceReport()) {
        return;
    }
    // fprintf+fflush rather than LOG(): the logger forks its sink. Setting
    // MRT_LOG_PATH sends every LOG() line to that path with a ".<pid>" suffix
    // appended (Log.cpp:194-200 GetLogPath, Log.cpp:426 FormatLog) and leaves
    // nothing on stderr, while fprintf-based lines (GCLOG, the f3-deadarm
    // report at WCollector.cpp:280) are unaffected. A control pair has to land
    // in one stream: otherwise flipping a harness env var scatters the open and
    // closed arms across two sinks and the comparison silently stops being one.
    //
    // NB LOG(RTLOG_ERROR) itself is fine -- it reaches stderr whenever
    // MRT_LOG_PATH is unset. Commit 1f9056ab's message claimed it "did not
    // reach stderr" as a general fact; that was wrong. See the docs commit
    // that added this comment.
    std::fprintf(stderr,
                 "[GCV2][partial-array] point=%s arrays_split=%zu chunks_pushed=%zu "
                 "chunks_followed=%zu not_encodable=%zu min_length=%zu enabled=%d\n",
                 point != nullptr ? point : "?",
                 static_cast<size_t>(g_arraysSplit.load(std::memory_order_relaxed)),
                 static_cast<size_t>(g_chunksPushed.load(std::memory_order_relaxed)),
                 static_cast<size_t>(g_chunksFollowed.load(std::memory_order_relaxed)),
                 static_cast<size_t>(g_notEncodable.load(std::memory_order_relaxed)),
                 MIN_LENGTH, Enabled() ? 1 : 0);
    std::fflush(stderr);
}

} // namespace MarkPartialArray
} // namespace MapleRuntime
