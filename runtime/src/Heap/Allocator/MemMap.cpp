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
namespace {

constexpr size_t kDefaultSafeFraction = 2;
constexpr unsigned long kMaxNumaNodes = sizeof(unsigned long) * 8;
constexpr int kMpolBind = 2;
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
        return result;
#endif
    }

    bool Commit(void* addr, size_t size, int prot, uint32_t numaNode, bool bindNuma) override
    {
#ifdef _WIN64
        (void)prot;
        (void)numaNode;
        (void)bindNuma;
        return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
#if defined(__linux__) && defined(SYS_mbind)
        if (bindNuma) {
            if (numaNode >= kMaxNumaNodes) {
                return false;
            }
            unsigned long mask = 1UL << numaNode;
            const long rc = syscall(SYS_mbind, addr, size, kMpolBind, &mask, kMaxNumaNodes, 0UL);
            if (rc != 0) {
                return false;
            }
        }
#else
        (void)numaNode;
        (void)bindNuma;
#endif
        return mprotect(addr, size, prot) == 0;
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

    bool Release(void* addr, size_t size, uint32_t numaNode) override
    {
        (void)numaNode;
#ifdef _WIN64
        return VirtualFree(addr, size, MEM_DECOMMIT) != 0;
#elif defined(__APPLE__)
        return madvise(addr, size, MADV_FREE) == 0;
#else
        return madvise(addr, size, MADV_DONTNEED) == 0;
#endif
    }

    bool Unreserve(void* addr, size_t size) override
    {
#ifdef _WIN64
        (void)size;
        return VirtualFree(addr, 0, MEM_RELEASE) != 0;
#else
        return munmap(addr, size) == 0;
#endif
    }
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

bool NumaPartitionRegistry::Initialize(uintptr_t start, size_t size, const NumaTopology& topology)
{
    if (!topology.IsSealed() || start == 0 || size == 0 || AddOverflows(start, size) ||
        (size % ALLOC_UTIL_PAGE_SIZE) != 0) {
        return false;
    }
    const size_t granules = size / ALLOC_UTIL_PAGE_SIZE;
    const size_t count = std::min(topology.Count(), granules);
    if (count == 0) {
        return false;
    }
    uintptr_t cursor = start;
    for (size_t index = 0; index < count; ++index) {
        const size_t nodeGranules = granules / count + (index < granules % count ? 1 : 0);
        const size_t nodeBytes = nodeGranules * ALLOC_UTIL_PAGE_SIZE;
        ranges.push_back(NumaPartitionRange{ MemoryRange{ cursor, nodeBytes }, topology.NodeAt(index) });
        cursor += nodeBytes;
    }
    return cursor == start + size;
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
        size_t segmentSize = std::min(fallbackSegmentSize, mappedSize);
        segmentSize = AllocUtilRndDown(segmentSize, static_cast<size_t>(ALLOC_UTIL_PAGE_SIZE));
        if (segmentSize == 0) {
            segmentSize = ALLOC_UTIL_PAGE_SIZE;
        }
        size_t reserved = 0;
        while (reserved < mappedSize) {
            const size_t currentSize = std::min(segmentSize, mappedSize - reserved);
            void* requested = reserved == 0 ? opt.reqBase
                                            : reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(base) + reserved);
            void* segment = osBackend.Reserve(requested, currentSize, opt.flags, opt.tag,
                                               reserved != 0 || requested != nullptr);
            if (segment == nullptr || (reserved != 0 && segment != requested) ||
                !registry.Insert(MemoryRange{ reinterpret_cast<uintptr_t>(segment), currentSize })) {
                if (segment != nullptr) {
                    (void)osBackend.Unreserve(segment, currentSize);
                }
                for (const MemoryRange& range : registry.Ranges()) {
                    (void)osBackend.Unreserve(reinterpret_cast<void*>(range.start), range.size);
                }
                return nullptr;
            }
            if (reserved == 0) {
                base = segment;
            }
            reserved += currentSize;
        }
    }

    NumaPartitionRegistry partitions;
    if (!partitions.Initialize(reinterpret_cast<uintptr_t>(base), mappedSize, topology)) {
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
    if (initialCommit != 0 && !memMap->CommitMemory(base, initialCommit)) {
        delete memMap;
        return nullptr;
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
    memMappedEndAddr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(memBaseAddr) + memMappedSize);
}

bool MemMap::ApplyByPartition(void* addr, size_t size, uint32_t* requiredNode, bool release)
{
    const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    if (!IsValidRange(start, size) || !reservationRegistry.Contains(start, size)) {
        return false;
    }
    // Validate an explicit owner before the first backend call.  A cross-node
    // free must be rejected atomically, not after partially releasing one side.
    if (requiredNode != nullptr && !numaPartitions.Owns(start, size, *requiredNode)) {
        return false;
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
            return false;
        }
        const bool ok = release ? backend->Release(reinterpret_cast<void*>(partStart), partEnd - partStart,
                                                   partition.node)
                                : backend->Commit(reinterpret_cast<void*>(partStart), partEnd - partStart, commitProt,
                                                  partition.node, bindNuma);
        if (!ok) {
            return false;
        }
        cursor = partEnd;
        if (cursor == end) {
            return true;
        }
    }
    return false;
}

bool MemMap::CommitMemory(void* addr, size_t size)
{
    return ApplyByPartition(addr, size, nullptr, false);
}

bool MemMap::CommitMemory(void* addr, size_t size, uint32_t numaNode)
{
    return ApplyByPartition(addr, size, &numaNode, false);
}

bool MemMap::ReleaseMemory(void* addr, size_t size)
{
    return ApplyByPartition(addr, size, nullptr, true);
}

bool MemMap::ReleaseMemory(void* addr, size_t size, uint32_t numaNode)
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
