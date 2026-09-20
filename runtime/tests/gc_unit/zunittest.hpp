// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Port of test/hotspot/gtest/gc/z/zunittest.hpp:40-201: address-domain
// fixtures for the memory-manager tests. ZTest::ZAddressReserver reserves
// heap address space through ZVirtualMemoryReserver; ZPhysicalMemoryBackingMocker
// hands out a backing file; ZTestHeapMapping combines both into the committed,
// mapped fixture heap the ZGC tests build in test_zForwarding.cpp:70-86.

#pragma once
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <ostream>
#include <sys/mman.h>

#include "gc_unittest.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zPhysicalMemoryManager.hpp"
#include "Heap/z/zRangeRegistry.inline.hpp"
#include "Heap/z/zVirtualMemory.inline.hpp"
#include "Heap/z/zVirtualMemoryManager.inline.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zPageTable.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zGeneration.hpp"
#include <vector>

namespace MapleRuntime {

inline std::ostream& operator<<(std::ostream& str, const ZVirtualMemory& vmem) {
  return str << "ZVirtualMemory{start=" << (void*)untype(vmem.start()) << ", size=" << vmem.size() << "}";
}

// zunittest.hpp:178-189: the address contract is established once per process.
inline void EnsureZAddressDomain() {
  if (ZAddressHeapBase == 0) {
    ZGlobalsPointers::initialize();
  }
}

// ZGC zHeap.cpp:253-257: pages enter the table only from ZHeap::alloc_page.
inline void PublishAllocatedPage(ZPage* page)
{
    Heap::alloc_page(page);
}

// Heap already owns page_table/forwarding/remembered at construction
// (zGeneration.cpp:499-505). Fixtures must not replace those members.
class ZFixtureRememberedScope {
public:
    ZFixtureRememberedScope() { (void)Heap::GetHeap(); }
    explicit ZFixtureRememberedScope(RegionManager&) { (void)Heap::GetHeap(); }
};

class ZAddressOffsetMaxSetter {
  friend class ZTest;

private:
  size_t _old_max;
  size_t _old_mask;

public:
  ZAddressOffsetMaxSetter(size_t zaddress_offset_max)
    : _old_max(ZAddressOffsetMax),
      _old_mask(ZAddressOffsetMask) {
    ZAddressOffsetMax = zaddress_offset_max;
    ZAddressOffsetMask = ZAddressOffsetMax - 1;
  }
  ~ZAddressOffsetMaxSetter() {
    ZAddressOffsetMax = _old_max;
    ZAddressOffsetMask = _old_mask;
  }
};

class ZTest {
public:
  class ZAddressReserver {
    ZVirtualMemoryReserver* _reserver;
    bool _active;

    public:
      ZAddressReserver()
        : _reserver(nullptr),
          _active(false) {}

      ~ZAddressReserver() {
        if (_active) {
          std::fprintf(stderr, "ZAddressReserver deconstructed without calling TearDown\n");
        }
      }

      void SetUp(size_t reservation_size) {
        GC_EXPECT_FALSE(_active);
        _active = true;

        EnsureZAddressDomain();
        _reserver = new ZVirtualMemoryReserver(reservation_size);
      }

      void TearDown() {
        GC_EXPECT_TRUE(_active);
        _active = false;

        // Best-effort cleanup
        _reserver->unreserve_all();
        delete _reserver;
        _reserver = nullptr;
      }

      ZVirtualMemoryReserver* reserver() {
        GC_EXPECT_TRUE(_active);
        return _reserver;
      }

      ZVirtualMemoryRegistry* registry() {
        GC_EXPECT_TRUE(_active);
        return &_reserver->_registry;
      }
  };

  class ZPhysicalMemoryBackingMocker {
    size_t                  _old_max;
    ZPhysicalMemoryBacking* _backing;
    bool                    _active;

    static size_t set_max(size_t max_capacity) {
      size_t old_max = ZBackingOffsetMax;

      ZBackingOffsetMax = max_capacity;
      ZBackingIndexMax = static_cast<uint32_t>(ZBackingOffsetMax / ZGranuleSize);

      return old_max;
    }

  public:
    ZPhysicalMemoryBackingMocker()
      : _old_max(0),
        _backing(nullptr),
        _active(false) {}

    void SetUp(size_t max_capacity) {
      GC_EXPECT_FALSE(_active);

      _old_max = set_max(max_capacity);

      _backing = new ZPhysicalMemoryBacking(max_capacity);

      _active = true;
    }

    void TearDown() {
      GC_EXPECT_TRUE(_active);

      _active = false;

      delete _backing;
      _backing = nullptr;

      set_max(_old_max);
    }

    ZPhysicalMemoryBacking* operator()() {
      return _backing;
    }
  };

  // Saves the backing limits ZPhysicalMemoryManager's constructor installs
  // (zPhysicalMemoryManager.cpp:54-55) so fixtures can be torn down and rebuilt.
  class ZBackingLimitSetter {
    size_t   _old_offset_max;
    uint32_t _old_index_max;

  public:
    ZBackingLimitSetter()
      : _old_offset_max(ZBackingOffsetMax),
        _old_index_max(ZBackingIndexMax) {}
    ~ZBackingLimitSetter() {
      ZBackingOffsetMax = _old_offset_max;
      ZBackingIndexMax = _old_index_max;
    }
  };
};

// A committed, mapped range of `size` bytes inside the heap address domain:
// reserve (ZVirtualMemoryReserver) + backing commit + map, as the ZGC page
// fixtures do (test_zForwarding.cpp:70-86).
class ZTestHeapMapping {
public:
  explicit ZTestHeapMapping(size_t size)
    : _size(size) {
    _reserver.SetUp(size);
    GC_EXPECT_TRUE(_reserver.reserver()->reserved() == size && _reserver.registry()->is_contiguous());
    _offset = _reserver.registry()->peek_low_address();
    GC_EXPECT_TRUE(_offset != zoffset::invalid);
    _backing.SetUp(size);
    GC_EXPECT_TRUE(_backing()->is_initialized());
    const size_t committed = _backing()->commit(zbacking_offset(0), size, 0);
    GC_EXPECT_EQ(committed, size);
    _backing()->map(ZOffset::address_unsafe(_offset), size, zbacking_offset(0));
  }

  ~ZTestHeapMapping() {
    _backing()->unmap(ZOffset::address_unsafe(_offset), _size);
    _backing()->uncommit(zbacking_offset(0), _size);
    _backing.TearDown();
    _reserver.TearDown();
  }

  void* base() const { return reinterpret_cast<void*>(untype(ZOffset::address_unsafe(_offset))); }
  uintptr_t address() const { return untype(ZOffset::address_unsafe(_offset)); }
  zoffset offset() const { return _offset; }
  size_t size() const { return _size; }

private:
  ZTest::ZAddressReserver _reserver;
  ZTest::ZPhysicalMemoryBackingMocker _backing;
  zoffset _offset{ zoffset::invalid };
  size_t _size;
};

// Shared page fixtures borrow a contiguous extent from the product allocator.
// Subpage descriptors are test objects; the allocator retains the original
// extent's ownership. Global reservation/metadata state is never replaced.
class ZTestAllocatedMemory {
public:
    explicit ZTestAllocatedMemory(size_t size)
        : _owner(Heap::GetHeap().page_allocator().TakeRegion(size, ZPageType::large, false, false, true)),
          _size(size)
    {
        GC_EXPECT_TRUE(_owner != nullptr);
        _start = _owner->GetRegionStart();
    }

    ~ZTestAllocatedMemory()
    {
        for (size_t offset = 0; offset < _size;) {
            ZPage* page = Heap::page(_start + offset);
            if (page == nullptr) {
                offset += ZGranuleSize;
                continue;
            }
            const size_t bytes = page->size();
            Heap::page_table().remove(page);
            ZPage::RetireDescriptor(page);
            offset += bytes;
        }
        Heap::GetHeap().page_allocator().free_page(_owner);
    }

    void* base() const { return reinterpret_cast<void*>(_start); }

private:
    ZPage* _owner;
    size_t _size;
    uintptr_t _start;
};

// A RegionManager over the two memory managers, the way RegionSpace::Init
// builds the product heap (ZPageAllocator's constructor shape): managers for
// `units` of max capacity, the per-unit metadata over the reserved span,
// RegionManager::Initialize. Restores ZAddressOffsetMax and the backing
// limits on teardown so fixtures can be rebuilt in one process.
class ZTestRegionHeap {
public:
  ZTestRegionHeap(size_t units, RegionManager& manager, const HeapParam& params, double garbageThreshold)
    : _offsetMax(ZAddressOffsetMax) {
    EnsureZAddressDomain();
    const size_t maxCapacity = units * ZGranuleSize;
    _virtual.reset(new ZVirtualMemoryManager(maxCapacity));
    GC_EXPECT_TRUE(_virtual->is_initialized());
    _physical.reset(new ZPhysicalMemoryManager(maxCapacity));
    GC_EXPECT_TRUE(_physical->is_initialized());
    const std::vector<ZPage::ReservedSegment> segments = RegionManager::ReservedSegments(*_virtual);
    _metadataSize = RegionManager::GetMetadataSize();
    _metadata = mmap(nullptr, _metadataSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    GC_EXPECT_TRUE(_metadata != MAP_FAILED);
    manager.Initialize(units * ZGranuleSize, reinterpret_cast<uintptr_t>(_metadata), *_virtual, *_physical, params, garbageThreshold);
  }

  ~ZTestRegionHeap() {
    _physical.reset();
    _virtual.reset();
    if (_metadata != nullptr && _metadata != MAP_FAILED) {
      (void)munmap(_metadata, _metadataSize);
    }
  }

  ZVirtualMemoryManager& virtualMemory() { return *_virtual; }
  ZPhysicalMemoryManager& physicalMemory() { return *_physical; }
  uintptr_t metadata() const { return reinterpret_cast<uintptr_t>(_metadata); }

private:
  ZTest::ZBackingLimitSetter _backingLimits;
  ZAddressOffsetMaxSetter _offsetMax;
  std::unique_ptr<ZVirtualMemoryManager> _virtual;
  std::unique_ptr<ZPhysicalMemoryManager> _physical;
  void* _metadata{ nullptr };
  size_t _metadataSize{ 0 };
};

} // namespace MapleRuntime
