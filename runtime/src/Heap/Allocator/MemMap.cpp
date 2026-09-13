// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "MemMap.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
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

namespace MapleRuntime {
// C++14 default arguments bind this option by reference (metadata mapping).
constexpr MemMap::Option MemMap::DEFAULT_OPTIONS;

namespace {

constexpr size_t kDefaultSafeFraction = 2;
constexpr unsigned long kMaxNumaNodes = sizeof(unsigned long) * 8;
constexpr int kMpolPreferred = 1;
constexpr int kMpolMemsAllowed = 2;

bool AddOverflows(uintptr_t start, size_t size)
{
    return size > std::numeric_limits<uintptr_t>::max() - start;
}

class NativeMemMapBackend final : public MemMapBackend {
public:
    void* Reserve(void* requested, size_t size, unsigned int flags, const char* tag, bool exact) override
    {
#ifdef _WIN64
        (void)flags;
        (void)tag;
        (void)exact;
        return VirtualAlloc(requested, size, MEM_RESERVE, PAGE_NOACCESS);
#else
        int mmapFlags = static_cast<int>(flags);
#if defined(MAP_FIXED_NOREPLACE)
        if (exact) {
            mmapFlags |= MAP_FIXED_NOREPLACE;
        }
#endif
#if defined(__APPLE__)
        int fd = -1;
        if (IsCangjieHeapTag(tag)) {
            mmapFlags &= ~MAP_NORESERVE;
            fd = VM_MAKE_TAG(CANGJIE_HEAP_VM_TAG);
        }
        void* result = mmap(requested, size, PROT_NONE, mmapFlags, fd, 0);
#else
        void* result = mmap(requested, size, PROT_NONE, mmapFlags, -1, 0);
#endif
        if (result == MAP_FAILED) {
            return nullptr;
        }
        if (exact && result != requested) {
            (void)munmap(result, size);
            return nullptr;
        }
#if !defined(__APPLE__)
        (void)madvise(result, size, MADV_NOHUGEPAGE);
        MRT_PRCTL(result, size, tag);
#endif
#if defined(__linux__)
        // zPhysicalMemoryBacking_linux.cpp: create_fd/fallocate/map. Each
        // reservation owns its backing file; offsets survive decommit.
        const int fd = static_cast<int>(syscall(SYS_memfd_create, "cangjie-heap", 1U));
        if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
            if (fd >= 0) { close(fd); }
            munmap(result, size);
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(filesMutex);
        files.push_back(BackingFile{ reinterpret_cast<uintptr_t>(result), size, fd });
#endif
        return result;
#endif
    }

    size_t Commit(void* addr, size_t size, int prot, uint32_t numaNode, bool bindNuma) override
    {
#ifdef _WIN64
        (void)prot;
        (void)numaNode;
        (void)bindNuma;
        return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr ? size : 0;
#elif defined(__linux__)
        std::lock_guard<std::mutex> lock(filesMutex);
        const BackingFile* file = FindFile(addr, size);
        if (file == nullptr) { return 0; }
        // zPhysicalMemoryBacking_linux.cpp:627: policy applies while allocating
        // backing, and is restored afterwards. NUMA preference is not a strict
        // binding requirement: an unavailable preferred node can fall back.
        if (bindNuma && numaNode < kMaxNumaNodes) {
            unsigned long mask = 1UL << numaNode;
            if (syscall(SYS_set_mempolicy, kMpolPreferred, &mask, kMaxNumaNodes) != 0) {
                LOG(RTLOG_WARNING, "backing NUMA preference failed: %d", errno);
            }
        }
        const size_t offset = reinterpret_cast<uintptr_t>(addr) - file->start;
        const size_t committed = CommitFile(file->fd, offset, size);
        if (bindNuma) {
            if (syscall(SYS_set_mempolicy, kMpolPreferred, nullptr, 0UL) != 0) {
                LOG(RTLOG_WARNING, "backing NUMA preference reset failed: %d", errno);
            }
        }
        if (committed != 0) {
            void* mapped = mmap(addr, committed, prot, MAP_SHARED | MAP_FIXED, file->fd, offset);
            CHECK_DETAIL(mapped == addr, "failed to map committed backing: %d", errno);
        }
        return committed;
#else
        (void)numaNode;
        (void)bindNuma;
        return mprotect(addr, size, prot) == 0 ? size : 0;
#endif
    }

    bool Protect(void* addr, size_t size, int prot) override
    {
#ifdef _WIN64
        DWORD oldProtect = 0;
        DWORD newProtect = PAGE_NOACCESS;
        if ((prot & MemMap::PROT_EXEC) != 0) {
            newProtect = (prot & MemMap::PROT_WRITE) != 0 ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
        } else if ((prot & MemMap::PROT_WRITE) != 0) {
            newProtect = PAGE_READWRITE;
        } else if ((prot & MemMap::PROT_READ) != 0) {
            newProtect = PAGE_READONLY;
        }
        return VirtualProtect(addr, size, newProtect, &oldProtect) != 0;
#else
        return mprotect(addr, size, prot) == 0;
#endif
    }

    size_t Release(void* addr, size_t size, uint32_t numaNode) override
    {
        (void)numaNode;
#ifdef _WIN64
        return VirtualFree(addr, size, MEM_DECOMMIT) != 0 ? size : 0;
#elif defined(__APPLE__)
        return madvise(addr, size, MADV_FREE) == 0 ? size : 0;
#elif defined(__linux__)
        std::lock_guard<std::mutex> lock(filesMutex);
        const BackingFile* file = FindFile(addr, size);
        if (file == nullptr) { return 0; }
        const size_t offset = reinterpret_cast<uintptr_t>(addr) - file->start;
        if (fallocate(file->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                      static_cast<off_t>(offset), static_cast<off_t>(size)) != 0) {
            LOG(RTLOG_ERROR, "failed to uncommit backing: %d", errno);
            return 0;
        }
        return size;
#else
        return madvise(addr, size, MADV_DONTNEED) == 0 ? size : 0;
#endif
    }

    bool Unreserve(void* addr, size_t size) override
    {
#ifdef _WIN64
        (void)size;
        return VirtualFree(addr, 0, MEM_RELEASE) != 0;
#else
        if (munmap(addr, size) != 0) { return false; }
#if defined(__linux__)
        std::lock_guard<std::mutex> lock(filesMutex);
        for (auto it = files.begin(); it != files.end(); ++it) {
            if (it->start == reinterpret_cast<uintptr_t>(addr) && it->size == size) {
                close(it->fd);
                files.erase(it);
                break;
            }
        }
#endif
        return true;
#endif
    }

private:
#if defined(__linux__)
    struct BackingFile { uintptr_t start; size_t size; int fd; };
    std::mutex filesMutex;
    std::vector<BackingFile> files;

    const BackingFile* FindFile(void* addr, size_t size) const
    {
        const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
        for (const auto& file : files) {
            if (start >= file.start && start - file.start <= file.size &&
                size <= file.size - (start - file.start)) { return &file; }
        }
        return nullptr;
    }

    static size_t CommitFile(int fd, size_t offset, size_t size)
    {
        // zPhysicalMemoryBacking_linux.cpp:639: whole range, then binary
        // subdivision retaining every successful granule-aligned prefix.
        if (fallocate(fd, 0, offset, size) == 0) { return size; }
        LOG(RTLOG_ERROR, "failed to commit whole backing range: %d", errno);
        size_t start = 0;
        size_t end = size;
        for (;;) {
            const size_t length = AllocUtilRndDown((end - start) / 2,
                                                  static_cast<size_t>(ALLOC_UTIL_PAGE_SIZE));
            if (length == 0) { break; }
            if (fallocate(fd, 0, offset + start, length) == 0) {
                start += length;
            } else {
                end -= length;
            }
        }
        // A failed filesystem allocation may have populated part of its
        // range. Discard only the suffix not included in the returned prefix.
        CHECK_DETAIL(fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                               offset + start, size - start) == 0,
                     "failed to release uncommitted backing suffix: %d", errno);
        return start;
    }
#endif
};

NativeMemMapBackend& NativeBackend()
{
    static NativeMemMapBackend backend;
    return backend;
}

} // namespace

AddressSpaceBudget AddressSpaceBudget::Seal(size_t available, size_t safeFraction)
{
    AddressSpaceBudget budget;
    if (safeFraction == 0) {
        return budget;
    }
    budget.availableBytes = available;
    budget.safeBytes = available / safeFraction;
    budget.sealed = true;
    return budget;
}

AddressSpaceBudget AddressSpaceBudget::SealProcessBudget()
{
#ifdef _WIN64
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) {
        return Seal(0, kDefaultSafeFraction);
    }
    return Seal(static_cast<size_t>(status.ullTotalVirtual), kDefaultSafeFraction);
#else
    struct rlimit limit {};
    if (getrlimit(RLIMIT_AS, &limit) != 0) {
        return Seal(0, kDefaultSafeFraction);
    }
    const size_t available = limit.rlim_cur == RLIM_INFINITY ? std::numeric_limits<size_t>::max()
                                                             : static_cast<size_t>(limit.rlim_cur);
    return Seal(available, kDefaultSafeFraction);
#endif
}

NumaTopology NumaTopology::Seal(const std::vector<uint32_t>& nodeIds)
{
    NumaTopology topology;
    topology.nodes = nodeIds;
    std::sort(topology.nodes.begin(), topology.nodes.end());
    topology.nodes.erase(std::unique(topology.nodes.begin(), topology.nodes.end()), topology.nodes.end());
    if (topology.nodes.empty()) {
        topology.nodes.push_back(0);
    }
    topology.sealed = true;
    return topology;
}

NumaTopology NumaTopology::SealProcessTopology()
{
    std::vector<uint32_t> nodes;
#if defined(__linux__) && defined(SYS_get_mempolicy)
    unsigned long mask = 0;
    const long rc = syscall(SYS_get_mempolicy, nullptr, &mask, kMaxNumaNodes, nullptr, kMpolMemsAllowed);
    if (rc == 0) {
        for (uint32_t node = 0; node < kMaxNumaNodes; ++node) {
            if ((mask & (1UL << node)) != 0) {
                nodes.push_back(node);
            }
        }
    }
#endif
    return Seal(nodes);
}

bool NumaTopology::Contains(uint32_t node) const
{
    return std::binary_search(nodes.begin(), nodes.end(), node);
}

bool ReservationRegistry::Insert(MemoryRange range)
{
    if (range.IsNull() || AddOverflows(range.start, range.size)) {
        return false;
    }
    auto pos = std::lower_bound(ranges.begin(), ranges.end(), range.start,
        [](const MemoryRange& left, uintptr_t start) { return left.start < start; });
    if (pos != ranges.begin() && (pos - 1)->End() > range.start) {
        return false;
    }
    if (pos != ranges.end() && range.End() > pos->start) {
        return false;
    }
    ranges.insert(pos, range);
    return true;
}

bool ReservationRegistry::Contains(uintptr_t start, size_t size) const
{
    if (start == 0 || size == 0 || AddOverflows(start, size)) {
        return false;
    }
    const uintptr_t end = start + size;
    uintptr_t cursor = start;
    for (const MemoryRange& range : ranges) {
        if (range.End() <= cursor) {
            continue;
        }
        if (range.start > cursor) {
            return false;
        }
        cursor = std::min(end, range.End());
        if (cursor == end) {
            return true;
        }
    }
    return false;
}

size_t ReservationRegistry::TotalSize() const
{
    size_t total = 0;
    for (const MemoryRange& range : ranges) {
        total += range.size;
    }
    return total;
}

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

bool MemMap::IsValidRange(uintptr_t start, size_t size)
{
    return start != 0 && size != 0 && !AddOverflows(start, size);
}

MemMap* MemMap::TryMapMemory(size_t reqSize, size_t initSize, const Option& opt,
                            const AddressSpaceBudget& budget, const NumaTopology& topology,
                            MemMapBackend& osBackend, size_t fallbackSegmentSize)
{
    if (reqSize == 0 || initSize > reqSize || !budget.IsSealed() || !topology.IsSealed() ||
        reqSize > std::numeric_limits<size_t>::max() - (ALLOC_UTIL_PAGE_SIZE - 1)) {
        return nullptr;
    }
    const size_t mappedSize = AllocUtilRndUp<size_t>(reqSize, ALLOC_UTIL_PAGE_SIZE);
    if (!budget.Allows(mappedSize)) {
        return nullptr;
    }

    ReservationRegistry registry;
    void* base = osBackend.Reserve(opt.reqBase, mappedSize, opt.flags, opt.tag, opt.reqBase != nullptr);
    if (base != nullptr) {
        if (!registry.Insert(MemoryRange{ reinterpret_cast<uintptr_t>(base), mappedSize })) {
            (void)osBackend.Unreserve(base, mappedSize);
            return nullptr;
        }
    } else {
        if (opt.reqBase != nullptr) {
            return nullptr;
        }
        size_t segmentSize = std::min(fallbackSegmentSize, mappedSize);
        segmentSize = AllocUtilRndDown(segmentSize, static_cast<size_t>(ALLOC_UTIL_PAGE_SIZE));
        if (segmentSize == 0) {
            segmentSize = ALLOC_UTIL_PAGE_SIZE;
        }
        size_t reserved = 0;
        while (reserved < mappedSize) {
            const size_t currentSize = std::min(segmentSize, mappedSize - reserved);
            void* segment = osBackend.Reserve(nullptr, currentSize, opt.flags, opt.tag, false);
            if (segment == nullptr ||
                !registry.Insert(MemoryRange{ reinterpret_cast<uintptr_t>(segment), currentSize })) {
                if (segment != nullptr) {
                    (void)osBackend.Unreserve(segment, currentSize);
                }
                for (const MemoryRange& range : registry.Ranges()) {
                    (void)osBackend.Unreserve(reinterpret_cast<void*>(range.start), range.size);
                }
                return nullptr;
            }
            reserved += currentSize;
        }
        base = reinterpret_cast<void*>(registry.Ranges().front().start);
    }

    NumaPartitionRegistry partitions;
    if (!partitions.Initialize(registry, topology)) {
        for (const MemoryRange& range : registry.Ranges()) {
            (void)osBackend.Unreserve(reinterpret_cast<void*>(range.start), range.size);
        }
        return nullptr;
    }

    MemMap* memMap = new (std::nothrow) MemMap(base, initSize, mappedSize, opt.prot, std::move(registry),
                                               std::move(partitions), osBackend, topology.Count() > 1);
    if (memMap == nullptr) {
        // Placement new failed before ownership transferred.
        for (const MemoryRange& range : registry.Ranges()) {
            (void)osBackend.Unreserve(reinterpret_cast<void*>(range.start), range.size);
        }
        return nullptr;
    }
    const size_t initialCommit = opt.protAll ? mappedSize : initSize;
    if (initialCommit != 0) {
        const size_t committed = memMap->CommitMemory(base, initialCommit);
        if (committed != initialCommit) {
            // Commit proceeds partition by partition.  If only a prefix was
            // committed, release exactly that prefix before tearing down the
            // reservation so allocation failure leaves no hidden side effect.
            if (committed != 0) {
                (void)memMap->ReleaseMemory(base, committed);
            }
            delete memMap;
            return nullptr;
        }
    }
    return memMap;
}

MemMap* MemMap::MapMemory(size_t reqSize, size_t initSize, const Option& opt)
{
    const AddressSpaceBudget budget = AddressSpaceBudget::SealProcessBudget();
    const NumaTopology topology = NumaTopology::SealProcessTopology();
    return MapMemory(reqSize, initSize, opt, budget, topology);
}

MemMap* MemMap::MapMemory(size_t reqSize, size_t initSize, const Option& opt,
                         const AddressSpaceBudget& budget, const NumaTopology& topology)
{
    MemMap* memMap = TryMapMemory(reqSize, initSize, opt, budget, topology, NativeBackend());
    CHECK_DETAIL(memMap != nullptr, "MemMap::MapMemory failed reqSize: %zu initSize: %zu budget: %zu",
                 reqSize, initSize, budget.SafeBytes());
    return memMap;
}

MemMap::MemMap(void* baseAddr, size_t initSize, size_t mappedSize, int prot, ReservationRegistry&& registry,
               NumaPartitionRegistry&& partitions, MemMapBackend& osBackend, bool shouldBindNuma)
    : memBaseAddr(baseAddr), memCurrSize(initSize), memMappedSize(mappedSize), commitProt(prot),
      reservationRegistry(std::move(registry)), numaPartitions(std::move(partitions)), backend(&osBackend),
      bindNuma(shouldBindNuma)
{
    memCurrEndAddr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(memBaseAddr) + memCurrSize);
    memMappedEndAddr = reinterpret_cast<void*>(reservationRegistry.Ranges().back().End());
}

size_t MemMap::ApplyByPartition(void* addr, size_t size, uint32_t* requiredNode, bool release)
{
    std::lock_guard<std::mutex> lock(backingMutex);
    const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    if (!IsValidRange(start, size) || !reservationRegistry.Contains(start, size)) {
        return 0;
    }
    // Validate an explicit owner before the first backend call.  A cross-node
    // free must be rejected atomically, not after partially releasing one side.
    if (requiredNode != nullptr && !numaPartitions.Owns(start, size, *requiredNode)) {
        return 0;
    }
    const uintptr_t end = start + size;
    uintptr_t cursor = start;
    for (const NumaPartitionRange& partition : numaPartitions.Ranges()) {
        const uintptr_t partStart = std::max(cursor, partition.range.start);
        const uintptr_t partEnd = std::min(end, partition.range.End());
        if (partStart >= partEnd) {
            continue;
        }
        if (partStart != cursor || (requiredNode != nullptr && *requiredNode != partition.node)) {
            return static_cast<size_t>(cursor - start);
        }
        while (cursor < partEnd) {
            uintptr_t operationEnd = partEnd;
            if (!release) {
                // zPageAllocator.cpp:1880: harvested backing participates in
                // the successful prefix but must not be committed again.
                bool harvested = false;
                for (const auto& range : committedRanges) {
                    if (range.End() <= cursor) { continue; }
                    if (range.start <= cursor) {
                        cursor = std::min(partEnd, range.End());
                        harvested = true;
                    } else {
                        operationEnd = std::min(partEnd, range.start);
                    }
                    break;
                }
                if (harvested) { continue; }
            }
            const size_t requested = operationEnd - cursor;
            const size_t completed = release
                ? backend->Release(reinterpret_cast<void*>(cursor), requested, partition.node)
                : backend->Commit(reinterpret_cast<void*>(cursor), requested, commitProt, partition.node, bindNuma);
            CHECK(completed <= requested);
            RecordBacking(cursor, completed, release);
            cursor += completed;
            if (completed != requested) {
                return static_cast<size_t>(cursor - start);
            }
        }
        if (cursor == end) {
            return size;
        }
    }
    return static_cast<size_t>(cursor - start);
}

// zNMT.cpp:60-65: register exactly the completed backing range. This is
// also the page allocator's capacity source, including retained prefixes.
void MemMap::RecordBacking(uintptr_t start, size_t size, bool release)
{
    if (size == 0) { return; }
    const uintptr_t end = start + size;
    std::vector<MemoryRange> updated;
    for (const auto& range : committedRanges) {
        if (range.End() <= start || range.start >= end) {
            updated.push_back(range);
            continue;
        }
        if (range.start < start) { updated.push_back({ range.start, start - range.start }); }
        if (range.End() > end) { updated.push_back({ end, range.End() - end }); }
    }
    if (!release) { updated.push_back({ start, size }); }
    std::sort(updated.begin(), updated.end(), [](const MemoryRange& a, const MemoryRange& b) {
        return a.start < b.start;
    });
    committedRanges.clear();
    for (const auto& range : updated) {
        if (!committedRanges.empty() && committedRanges.back().End() == range.start) {
            committedRanges.back().size += range.size;
        } else {
            committedRanges.push_back(range);
        }
    }
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

MemMap::~MemMap()
{
    for (const MemoryRange& range : reservationRegistry.Ranges()) {
        if (!backend->Unreserve(reinterpret_cast<void*>(range.start), range.size)) {
            LOG(RTLOG_ERROR, "MemMap unreserve failed at %p size %zu", reinterpret_cast<void*>(range.start), range.size);
        }
    }
    memBaseAddr = nullptr;
    memCurrEndAddr = nullptr;
    memMappedEndAddr = nullptr;
}
} // namespace MapleRuntime
