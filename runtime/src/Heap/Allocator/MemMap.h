// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_ALLOC_MEM_MAP_H
#define MRT_ALLOC_MEM_MAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef _WIN64
#include <handleapi.h>
#include <memoryapi.h>
#else
#include <sys/mman.h>
#endif

#include "AllocUtil.h"
#include "Common/TypeDef.h"

namespace MapleRuntime {

class AddressSpaceBudget {
public:
    static AddressSpaceBudget Seal(size_t availableBytes, size_t safeFraction = 2);
    static AddressSpaceBudget SealProcessBudget();

    bool IsSealed() const { return sealed; }
    bool Allows(size_t bytes) const { return sealed && bytes <= safeBytes; }
    size_t AvailableBytes() const { return availableBytes; }
    size_t SafeBytes() const { return safeBytes; }

private:
    size_t availableBytes{ 0 };
    size_t safeBytes{ 0 };
    bool sealed{ false };
};

class NumaTopology {
public:
    static NumaTopology Seal(const std::vector<uint32_t>& nodeIds);
    static NumaTopology SealProcessTopology();

    bool IsSealed() const { return sealed; }
    size_t Count() const { return nodes.size(); }
    uint32_t NodeAt(size_t index) const { return nodes[index]; }
    bool Contains(uint32_t node) const;

private:
    std::vector<uint32_t> nodes;
    bool sealed{ false };
};

struct MemoryRange {
    uintptr_t start{ 0 };
    size_t size{ 0 };

    uintptr_t End() const { return start + size; }
    bool IsNull() const { return start == 0 || size == 0; }
};

class ReservationRegistry {
public:
    bool Insert(MemoryRange range);
    bool Contains(uintptr_t start, size_t size) const;
    size_t TotalSize() const;
    const std::vector<MemoryRange>& Ranges() const { return ranges; }

private:
    std::vector<MemoryRange> ranges;
};

struct NumaPartitionRange {
    MemoryRange range;
    uint32_t node{ 0 };
};

class NumaPartitionRegistry {
public:
    bool Initialize(const ReservationRegistry& reservations, const NumaTopology& topology);
    bool Owns(uintptr_t start, size_t size, uint32_t node) const;
    const std::vector<NumaPartitionRange>& Ranges() const { return ranges; }

private:
    std::vector<NumaPartitionRange> ranges;
};

class MemMapBackend {
public:
    virtual ~MemMapBackend() = default;
    virtual void* Reserve(void* requested, size_t size, unsigned int flags, const char* tag, bool exact) = 0;
    virtual bool Commit(void* addr, size_t size, int prot, uint32_t numaNode, bool bindNuma) = 0;
    virtual bool Protect(void* addr, size_t size, int prot) = 0;
    virtual bool Release(void* addr, size_t size, uint32_t numaNode) = 0;
    virtual bool Unreserve(void* addr, size_t size) = 0;
};

class MemMap {
public:
#ifdef _WIN64
    static constexpr int MAP_PRIVATE = 2;
    static constexpr int MAP_FIXED = 0x10;
    static constexpr int MAP_ANONYMOUS = 0x20;
    static constexpr int PROT_NONE = 0;
    static constexpr int PROT_READ = 1;
    static constexpr int PROT_WRITE = 2;
    static constexpr int PROT_EXEC = 4;
    static constexpr int DEFAULT_MEM_FLAGS = MAP_PRIVATE | MAP_ANONYMOUS;
#else
    static constexpr int DEFAULT_MEM_FLAGS = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#endif
    static constexpr int DEFAULT_MEM_PROT = PROT_READ | PROT_WRITE;

    struct Option {
        const char* tag;
        void* reqBase;
        unsigned int flags;
        int prot;
        bool protAll;
    };
    static constexpr Option DEFAULT_OPTIONS = { "maple_unnamed", nullptr, DEFAULT_MEM_FLAGS, DEFAULT_MEM_PROT, false };

    static MemMap* MapMemory(size_t reqSize, size_t initSize, const Option& opt = DEFAULT_OPTIONS);
    static MemMap* MapMemory(size_t reqSize, size_t initSize, const Option& opt,
                             const AddressSpaceBudget& budget, const NumaTopology& topology);
    static MemMap* TryMapMemory(size_t reqSize, size_t initSize, const Option& opt,
                               const AddressSpaceBudget& budget, const NumaTopology& topology,
                               MemMapBackend& backend, size_t fallbackSegmentSize = 64U * 1024U * 1024U);

    static void DestroyMemMap(MemMap*& memMap) noexcept
    {
        if (memMap != nullptr) {
            delete memMap;
            memMap = nullptr;
        }
    }

    bool CommitMemory(void* addr, size_t size);
    bool CommitMemory(void* addr, size_t size, uint32_t numaNode);
    bool ReleaseMemory(void* addr, size_t size);
    bool ReleaseMemory(void* addr, size_t size, uint32_t numaNode);
    bool ProtectMemory(void* addr, size_t size, int prot);

    void* GetBaseAddr() const { return memBaseAddr; }
    void* GetCurrEnd() const { return memCurrEndAddr; }
    void* GetMappedEndAddr() const { return memMappedEndAddr; }
    size_t GetCurrSize() const { return memCurrSize; }
    size_t GetMappedSize() const { return memMappedSize; }
    const ReservationRegistry& GetReservationRegistry() const { return reservationRegistry; }
    const NumaPartitionRegistry& GetNumaPartitionRegistry() const { return numaPartitions; }

    ~MemMap();
    MemMap(const MemMap& that) = delete;
    MemMap(MemMap&& that) = delete;
    MemMap& operator=(const MemMap& that) = delete;
    MemMap& operator=(MemMap&& that) = delete;

private:
    static bool IsValidRange(uintptr_t start, size_t size);
    bool ApplyByPartition(void* addr, size_t size, uint32_t* requiredNode, bool release);

    void* memBaseAddr{ nullptr };
    void* memCurrEndAddr{ nullptr };
    void* memMappedEndAddr{ nullptr };
    size_t memCurrSize{ 0 };
    size_t memMappedSize{ 0 };
    int commitProt{ DEFAULT_MEM_PROT };
    ReservationRegistry reservationRegistry;
    NumaPartitionRegistry numaPartitions;
    MemMapBackend* backend{ nullptr };
    bool bindNuma{ false };

    MemMap(void* baseAddr, size_t initSize, size_t mappedSize, int prot, ReservationRegistry&& registry,
           NumaPartitionRegistry&& partitions, MemMapBackend& osBackend, bool shouldBindNuma);
};
} // namespace MapleRuntime
#endif // MRT_ALLOC_MEM_MAP_H
