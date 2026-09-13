// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zVirtualMemoryManager.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <cerrno>
#elif !defined(_WIN64)
#include <sys/resource.h>
#endif
#ifdef _WIN64
#include <errhandlingapi.h>
#include <handleapi.h>
#include <memoryapi.h>
#include <sysinfoapi.h>
#endif

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/Panic.h"
#include "Base/SysCall.h"

#include "Heap/z/zVirtualMemory.inline.hpp"

namespace MapleRuntime {
bool NumaPartitionRegistry::Initialize(const ReservationRegistry& reservations, const NumaTopology& topology)
{
    const size_t size = reservations.TotalSize();
    if (!topology.IsSealed() || size == 0 || (size % ALLOC_UTIL_PAGE_SIZE) != 0) {
        return false;
    }
    const size_t granules = size / ALLOC_UTIL_PAGE_SIZE;
    const size_t count = std::min(topology.Count(), granules);
    if (count == 0) {
        return false;
    }
    size_t nodeIndex = 0;
    size_t remainingForNode = (granules / count + (nodeIndex < granules % count ? 1 : 0)) *
                              ALLOC_UTIL_PAGE_SIZE;
    size_t assigned = 0;
    for (const MemoryRange& reservation : reservations.Ranges()) {
        if (reservation.IsNull() || AddOverflows(reservation.start, reservation.size) ||
            (reservation.start % ALLOC_UTIL_PAGE_SIZE) != 0 ||
            (reservation.size % ALLOC_UTIL_PAGE_SIZE) != 0) {
            return false;
        }
        uintptr_t cursor = reservation.start;
        size_t remaining = reservation.size;
        while (remaining != 0) {
            const size_t partSize = std::min(remaining, remainingForNode);
            ranges.push_back(NumaPartitionRange{ MemoryRange{ cursor, partSize }, topology.NodeAt(nodeIndex) });
            cursor += partSize;
            remaining -= partSize;
            assigned += partSize;
            remainingForNode -= partSize;
            if (remainingForNode == 0 && assigned != size) {
                ++nodeIndex;
                if (nodeIndex >= count) {
                    return false;
                }
                remainingForNode = (granules / count + (nodeIndex < granules % count ? 1 : 0)) *
                                   ALLOC_UTIL_PAGE_SIZE;
            }
        }
    }
    return assigned == size;
}

bool NumaPartitionRegistry::Owns(uintptr_t start, size_t size, uint32_t node) const
{
    if (start == 0 || size == 0 || AddOverflows(start, size)) {
        return false;
    }
    const uintptr_t end = start + size;
    for (const NumaPartitionRange& partition : ranges) {
        if (partition.node == node && start >= partition.range.start && end <= partition.range.End()) {
            return true;
        }
    }
    return false;
}

}
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zVirtualMemoryManager.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <cerrno>
#elif !defined(_WIN64)
#include <sys/resource.h>
#endif
#ifdef _WIN64
#include <errhandlingapi.h>
#include <handleapi.h>
#include <memoryapi.h>
#include <sysinfoapi.h>
#endif

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/Panic.h"
#include "Base/SysCall.h"

#include "Heap/z/zVirtualMemory.inline.hpp"

namespace MapleRuntime {
MemoryRange MemMap::FindFreeBacking(uintptr_t preferred, size_t size, uint32_t node) const
{
    // zPhysicalMemoryManager::alloc: backing indices are independent of the
    // mapped virtual address. The committed segment ledger is authoritative.
    std::vector<MemoryRange> occupied;
    for (const auto& range : committedRanges) { occupied.push_back({range.backing, range.size}); }
    std::sort(occupied.begin(), occupied.end(), [](const MemoryRange& a, const MemoryRange& b) {
        return a.start < b.start;
    });
    for (const auto& partition : numaPartitions.Ranges()) {
        if (partition.node != node) { continue; }
        uintptr_t cursor = partition.range.start;
        if (!backend->CanRemapBacking()) {
            if (preferred < cursor || preferred >= partition.range.End()) { continue; }
            cursor = preferred;
        }
        for (const auto& used : occupied) {
            if (used.End() <= cursor) { continue; }
            if (used.start >= partition.range.End()) { break; }
            if (used.start > cursor) { return {cursor, std::min(size, used.start - cursor)}; }
            cursor = used.End();
        }
        if (cursor < partition.range.End()) {
            return {cursor, std::min(size, partition.range.End() - cursor)};
        }
    }
    return {};
}

void MemMap::SplitBackingAt(uintptr_t address)
{
    for (size_t i = 0; i < committedRanges.size(); ++i) {
        auto& range = committedRanges[i];
        if (!range.mapped || address <= range.start || address >= range.End()) { continue; }
        const size_t prefix = address - range.start;
        const CommittedRange tail{address, range.size - prefix, range.backing + prefix, range.node, true};
        range.size = prefix;
        committedRanges.push_back(tail);
        return;
    }
}

size_t MemMap::ApplyByPartition(void* addr, size_t size, uint32_t* requiredNode, bool release, bool publish)
{
    std::lock_guard<std::mutex> lock(backingMutex);
    const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    if (!IsValidRange(start, size) || !reservationRegistry.Contains(start, size) ||
        (requiredNode != nullptr && !numaPartitions.Owns(start, size, *requiredNode))) { return 0; }
    uintptr_t cursor = start;
    const uintptr_t end = start + size;
    for (const auto& partition : numaPartitions.Ranges()) {
        const uintptr_t partStart = std::max(cursor, partition.range.start);
        const uintptr_t partEnd = std::min(end, partition.range.End());
        if (partStart >= partEnd) { continue; }
        if (partStart != cursor) { break; }
        while (cursor < partEnd) {
            SplitBackingAt(cursor);
            SplitBackingAt(partEnd);
            auto found = std::find_if(committedRanges.begin(), committedRanges.end(), [cursor](const CommittedRange& r) {
                return r.mapped && r.start == cursor;
            });
            if (release) {
                if (found == committedRanges.end()) {
                    uintptr_t next = partEnd;
                    for (const auto& r : committedRanges) {
                        if (r.mapped && r.start > cursor) { next = std::min(next, r.start); }
                    }
                    cursor = next;
                    continue;
                }
                const size_t requested = found->size;
                const size_t done = backend->Release(reinterpret_cast<void*>(found->backing), requested, found->node);
                CHECK(done <= requested);
                if (done != 0) { CHECK(backend->UnmapBacking(reinterpret_cast<void*>(cursor), done)); }
                if (publish) {
                    found->start += done;
                    found->backing += done;
                    found->size -= done;
                    if (found->size == 0) { committedRanges.erase(found); }
                } else if (done != 0) {
                    found->mapped = false;
                    if (done != requested) {
                        const CommittedRange tail{found->start + done, found->size - done,
                                                  found->backing + done, found->node, true};
                        found->size = done;
                        committedRanges.push_back(tail);
                    }
                }
                cursor += done;
                if (done != requested) { return cursor - start; }
            } else {
                if (found != committedRanges.end()) { cursor += found->size; continue; }
                uintptr_t next = partEnd;
                for (const auto& r : committedRanges) {
                    if (r.mapped && r.start > cursor) { next = std::min(next, r.start); }
                }
                const MemoryRange physical = FindFreeBacking(cursor, next - cursor, partition.node);
                if (physical.IsNull()) { return cursor - start; }
                const size_t done = backend->CommitBacking(reinterpret_cast<void*>(physical.start), physical.size,
                                                           commitProt, partition.node, bindNuma);
                CHECK(done <= physical.size);
                if (done != 0) {
                    CHECK(backend->MapBacking(reinterpret_cast<void*>(cursor), reinterpret_cast<void*>(physical.start),
                                              done, commitProt));
                    committedRanges.push_back({cursor, done, physical.start, partition.node, true});
                }
                cursor += done;
                if (done != physical.size) { return cursor - start; }
            }
        }
        if (cursor == end) { return size; }
    }
    return cursor - start;
}

bool MemMap::StashSegments(const std::vector<MemoryRange>& ranges, std::vector<BackingSegment>& stash)
{
    std::lock_guard<std::mutex> lock(backingMutex);
    CHECK(stash.empty());
    if (!backend->CanRemapBacking()) { return false; }
    // Validate the entire claim before the first unmap.
    for (const auto& source : ranges) {
        size_t present = 0;
        for (const auto& range : committedRanges) {
            if (!range.mapped) { continue; }
            const uintptr_t lo = std::max(source.start, range.start);
            const uintptr_t hi = std::min(source.End(), range.End());
            if (hi > lo) { present += hi - lo; }
        }
        CHECK(present == source.size);
    }
    for (const auto& source : ranges) {
        SplitBackingAt(source.start);
        SplitBackingAt(source.End());
        CHECK(backend->UnmapBacking(reinterpret_cast<void*>(source.start), source.size));
        for (auto& range : committedRanges) {
            if (!range.mapped || range.start < source.start || range.End() > source.End()) { continue; }
            stash.push_back({range.backing, range.size, range.node});
            range.mapped = false;
        }
    }
    // ZPhysicalMemoryManager::stash_segments sorts indices to coalesce maps.
    std::sort(stash.begin(), stash.end(), [](const BackingSegment& a, const BackingSegment& b) {
        return a.backing < b.backing;
    });
    return true;
}

void MemMap::RestoreSegments(const std::vector<MemoryRange>& ranges, const std::vector<BackingSegment>& stash)
{
    std::lock_guard<std::mutex> lock(backingMutex);
    size_t total = 0;
    for (const auto& range : ranges) { total += range.size; }
    size_t stashed = 0;
    for (const auto& segment : stash) { stashed += segment.size; }
    CHECK(total == stashed);
    for (const auto& target : ranges) {
        CHECK(reservationRegistry.Contains(target.start, target.size));
        for (const auto& existing : committedRanges) {
            CHECK(!existing.mapped || existing.End() <= target.start || existing.start >= target.End());
        }
    }
    // Each token names exactly one detached entry, which remains capacity-owned
    // throughout the virtual registry shuffle (zPhysicalMemoryManager.cpp:384).
    for (const auto& segment : stash) {
        auto entry = std::find_if(committedRanges.begin(), committedRanges.end(), [&](const CommittedRange& r) {
            return !r.mapped && r.backing == segment.backing && r.size == segment.size;
        });
        CHECK(entry != committedRanges.end());
        committedRanges.erase(entry);
    }
    size_t token = 0;
    size_t offset = 0;
    for (const auto& range : ranges) {
        uintptr_t cursor = range.start;
        while (cursor < range.End()) {
            CHECK(token < stash.size());
            const auto& segment = stash[token];
            const size_t amount = std::min(range.End() - cursor, segment.size - offset);
            CHECK(reservationRegistry.Contains(cursor, amount));
            CHECK(numaPartitions.Owns(cursor, amount, segment.node));
            CHECK(backend->MapBacking(reinterpret_cast<void*>(cursor),
                                      reinterpret_cast<void*>(segment.backing + offset), amount, commitProt));
            committedRanges.push_back({cursor, amount, segment.backing + offset, segment.node, true});
            cursor += amount;
            offset += amount;
            if (offset == segment.size) { ++token; offset = 0; }
        }
    }
    CHECK(token == stash.size() && offset == 0);
}

size_t MemMap::GetCommittedSize() const
{
    std::lock_guard<std::mutex> lock(backingMutex);
    size_t size = 0;
    for (const auto& range : committedRanges) { size += range.size; }
    return size;
}

size_t MemMap::GetCommittedSize(uintptr_t start, size_t size) const
{
    std::lock_guard<std::mutex> lock(backingMutex);
    if (AddOverflows(start, size)) { return 0; }
    size_t committed = 0;
    for (const auto& range : committedRanges) {
        if (!range.mapped) { continue; }
        const uintptr_t lo = std::max(start, range.start);
        const uintptr_t hi = std::min(start + size, range.End());
        if (hi > lo) { committed += hi - lo; }
    }
    return committed;
}

size_t MemMap::CommitMemory(void* addr, size_t size)
{
    return ApplyByPartition(addr, size, nullptr, false);
}

size_t MemMap::CommitMemory(void* addr, size_t size, uint32_t numaNode)
{
    return ApplyByPartition(addr, size, &numaNode, false);
}

size_t MemMap::ReleaseMemory(void* addr, size_t size)
{
    return ApplyByPartition(addr, size, nullptr, true);
}

size_t MemMap::ReleaseMemory(void* addr, size_t size, uint32_t numaNode)
{
    return ApplyByPartition(addr, size, &numaNode, true);
}

size_t MemMap::ReleaseMemoryDeferred(void* addr, size_t size)
{
    // zUncommitter.cpp:409: perform only the physical operation here.
    return ApplyByPartition(addr, size, nullptr, true, false);
}

size_t MemMap::PublishMemoryRelease(void* addr, size_t completed)
{
    // zUncommitter.cpp:415: called under the page allocator owner after rejoining.
    std::lock_guard<std::mutex> lock(backingMutex);
    const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    CHECK(!AddOverflows(start, completed));
    const uintptr_t end = start + completed;
    SplitBackingAt(start);
    SplitBackingAt(end);
    size_t released = 0;
    auto it = committedRanges.begin();
    while (it != committedRanges.end()) {
        if (it->start >= start && it->End() <= end) {
            released += it->size;
            it = committedRanges.erase(it);
        } else {
            ++it;
        }
    }
    return released;
}

bool MemMap::ProtectMemory(void* addr, size_t size, int prot)
{
    const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    if (!IsValidRange(start, size) || !reservationRegistry.Contains(start, size)) {
        return false;
    }
    const uintptr_t end = start + size;
    uintptr_t cursor = start;
    for (const MemoryRange& range : reservationRegistry.Ranges()) {
        const uintptr_t rangeStart = std::max(cursor, range.start);
        const uintptr_t rangeEnd = std::min(end, range.End());
        if (rangeStart >= rangeEnd) {
            continue;
        }
        if (rangeStart != cursor || !backend->Protect(reinterpret_cast<void*>(rangeStart), rangeEnd - rangeStart, prot)) {
            return false;
        }
        cursor = rangeEnd;
        if (cursor == end) {
            return true;
        }
    }
    return false;
}

}
