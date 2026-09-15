#include "Heap/z/zAddress.hpp"
#include <cstring>
#include "Heap/z/zVirtualMemory.inline.hpp"
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

namespace MapleRuntime {
// C++14 default arguments bind this option by reference (metadata mapping).
constexpr MemMap::Option MemMap::DEFAULT_OPTIONS;

namespace {

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
        struct statfs backingStat {};
        if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0 || fstatfs(fd, &backingStat) != 0 ||
            backingStat.f_bsize <= 0 || ALLOC_UTIL_PAGE_SIZE % backingStat.f_bsize != 0) {
            if (fd >= 0) { close(fd); }
            munmap(result, size);
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(filesMutex);
        files.push_back(BackingFile{ reinterpret_cast<uintptr_t>(result), size, fd,
                                     static_cast<size_t>(backingStat.f_bsize) });
#endif
        return result;
#endif
    }

    size_t Commit(void* addr, size_t size, int prot, uint32_t numaNode, bool bindNuma) override
    {
        const size_t committed = CommitBacking(addr, size, prot, numaNode, bindNuma);
        if (committed != 0) { CHECK(MapBacking(addr, addr, committed, prot)); }
        return committed;
    }

    size_t CommitBacking(void* addr, size_t size, int prot, uint32_t numaNode, bool bindNuma) override
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
        const size_t committed = CommitFile(*file, offset, size);
        if (bindNuma) {
            if (syscall(SYS_set_mempolicy, kMpolPreferred, nullptr, 0UL) != 0) {
                LOG(RTLOG_WARNING, "backing NUMA preference reset failed: %d", errno);
            }
        }
        (void)prot;
        return committed;
#else
        (void)numaNode;
        (void)bindNuma;
        return mprotect(addr, size, prot) == 0 ? size : 0;
#endif
    }

    bool CanRemapBacking() const override
    {
#if defined(__linux__)
        return true;
#else
        return false;
#endif
    }

    bool MapBacking(void* addr, void* backing, size_t size, int prot) override
    {
#if defined(__linux__)
        std::lock_guard<std::mutex> lock(filesMutex);
        const BackingFile* file = FindFile(backing, size);
        if (file == nullptr) { return false; }
        const size_t offset = reinterpret_cast<uintptr_t>(backing) - file->start;
        return mmap(addr, size, prot, MAP_SHARED | MAP_FIXED, file->fd, offset) == addr;
#else
        (void)size;
        (void)prot;
        return addr == backing;
#endif
    }

    bool UnmapBacking(void* addr, size_t size) override
    {
#if defined(__linux__)
        return mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
                    -1, 0) == addr;
#else
        (void)addr;
        (void)size;
        return true;
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
        const int error = Fallocate(*file, true, offset, size);
        if (error != 0) {
            LOG(RTLOG_ERROR, "failed to uncommit backing: %d", error);
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
    struct BackingFile { uintptr_t start; size_t size; int fd; size_t blockSize; };
    std::mutex filesMutex;
    std::vector<BackingFile> files;
    // z_fallocate_supported: capability cached under filesMutex, not an option.
    bool fallocateSupported{ true };

    const BackingFile* FindFile(void* addr, size_t size) const
    {
        const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
        for (const auto& file : files) {
            if (start >= file.start && start - file.start <= file.size &&
                size <= file.size - (start - file.start)) { return &file; }
        }
        return nullptr;
    }

    static int FillHoleCompat(const BackingFile& file, size_t offset, size_t size)
    {
        // zPhysicalMemoryBacking_linux.cpp:468: ordinary memfd pages use pwrite
        // to allocate each backing block without relying on madvise or touching
        // a mapping whose backing allocation has not yet succeeded.
        const uint8_t data = 0;
        for (size_t pos = offset; pos < offset + size; pos += file.blockSize) {
            const ssize_t written = pwrite(file.fd, &data, sizeof(data), static_cast<off_t>(pos));
            if (written == -1) { return errno; }
            if (written != static_cast<ssize_t>(sizeof(data))) { return EIO; }
        }
        return 0;
    }

    int FillHole(const BackingFile& file, size_t offset, size_t size)
    {
        // zPhysicalMemoryBacking_linux.cpp:509: only unsupported fallocate
        // selects compatibility allocation; other errors reach the caller.
        if (fallocateSupported) {
            if (fallocate(file.fd, 0, static_cast<off_t>(offset), static_cast<off_t>(size)) == 0) {
                return 0;
            }
            const int error = errno;
            if (error != ENOSYS && error != EOPNOTSUPP) { return error; }
            fallocateSupported = false;
        }
        return FillHoleCompat(file, offset, size);
    }

    int Fallocate(const BackingFile& file, bool punchHole, size_t offset, size_t size)
    {
        int error;
        if (punchHole) {
            const int mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;
            error = fallocate(file.fd, mode, static_cast<off_t>(offset), static_cast<off_t>(size)) == 0
                ? 0 : errno;
        } else {
            error = FillHole(file, offset, size);
        }
        // zPhysicalMemoryBacking_linux.cpp:559-592: split interrupted ranges
        // at backing block boundaries, for both commit and uncommit.
        if (error == EINTR && size > file.blockSize) {
            const size_t firstSize = AllocUtilRndUp(size / 2, file.blockSize);
            const int firstError = Fallocate(file, punchHole, offset, firstSize);
            if (firstError != 0) { return firstError; }
            return Fallocate(file, punchHole, offset + firstSize, size - firstSize);
        }
        return error;
    }

    bool CommitFileRange(const BackingFile& file, size_t offset, size_t size)
    {
        // zPhysicalMemoryBacking_linux.cpp:598: ordinary-page errors are
        // reported to commit_default, which can retain a successful prefix.
        const int error = Fallocate(file, false, offset, size);
        if (error != 0) {
            LOG(RTLOG_ERROR, "failed to commit backing: %d", error);
            return false;
        }
        return true;
    }

    size_t CommitFile(const BackingFile& file, size_t offset, size_t size)
    {
        // zPhysicalMemoryBacking_linux.cpp:639: whole range, then binary
        // subdivision retaining every successful granule-aligned prefix.
        if (CommitFileRange(file, offset, size)) { return size; }
        size_t start = 0;
        size_t end = size;
        for (;;) {
            const size_t length = AllocUtilRndDown((end - start) / 2,
                                                  static_cast<size_t>(ALLOC_UTIL_PAGE_SIZE));
            if (length == 0) { return start; }
            if (CommitFileRange(file, offset + start, length)) {
                start += length;
            } else {
                end -= length;
            }
        }
    }

#endif
};

NativeMemMapBackend& NativeBackend()
{
    static NativeMemMapBackend backend;
    return backend;
}

} // namespace

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

bool MemMap::IsValidRange(uintptr_t start, size_t size)
{
    return start != 0 && size != 0 && !AddOverflows(start, size);
}

// ZVirtualMemoryReserver::reserve_contiguous, zVirtualMemoryManager.cpp:169-203.
// Native metadata mappings keep their native address domain; heap reservations
// and each fallback segment use the same bounded heap-address search.
static void* ReserveHeapAddress(MemMapBackend& backend, size_t size, const MemMap::Option& opt)
{
    if (size > ZAddressOffsetMax) { return nullptr; }
    const uintptr_t end = ZAddressHeapBase + ZAddressOffsetMax;
    if (opt.reqBase != nullptr) {
        const uintptr_t start = reinterpret_cast<uintptr_t>(opt.reqBase);
        if (start < ZAddressHeapBase || start > end - size) { return nullptr; }
        return backend.Reserve(opt.reqBase, size, opt.flags, opt.tag, true);
    }
    const size_t granule = size_t(1) << 21;
    const size_t unused = ZAddressOffsetMax - size;
    const size_t increment = std::max(AllocUtilRndUp(unused / 8192, granule), granule);
    for (uintptr_t offset = 0; offset <= unused; offset += increment) {
        void* requested = reinterpret_cast<void*>(raw(ZOffset::address_unsafe(to_zoffset(offset))));
        void* result = backend.Reserve(requested, size, opt.flags, opt.tag, true);
        if (result == requested) { return result; }
        if (result != nullptr) { (void)backend.Unreserve(result, size); }
    }
    return nullptr;
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
    const bool heapDomain = std::strcmp(opt.tag, "cangjie_heap") == 0;
    void* base = heapDomain ? ReserveHeapAddress(osBackend, mappedSize, opt)
                            : osBackend.Reserve(opt.reqBase, mappedSize, opt.flags, opt.tag, opt.reqBase != nullptr);
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
            void* segment = heapDomain ? ReserveHeapAddress(osBackend, currentSize, opt)
                                       : osBackend.Reserve(nullptr, currentSize, opt.flags, opt.tag, false);
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
