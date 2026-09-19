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

// Preserve the generation-owned remset and its indexing tables together.
// A fixture must establish the same binding as Heap::Init before publishing
// old pages (ZGC zRemembered.cpp:385-387, zPageTable.cpp:67-76).
class ZFixtureRememberedScope {
public:
    ZFixtureRememberedScope()
        : _allocator(&Heap::GetHeap().page_allocator()),
          _pages(std::move(Heap::page_table())),
          _young(std::move(Heap::GetHeap().young().forwarding_table())),
          _old(std::move(Heap::GetHeap().old().forwarding_table())),
          _remembered(std::move(*Heap::GetHeap().young().remembered())) {}

    ~ZFixtureRememberedScope()
    {
        Heap::page_table() = std::move(_pages);
        Heap::GetHeap().young().forwarding_table() = std::move(_young);
        Heap::GetHeap().old().forwarding_table() = std::move(_old);
        *Heap::GetHeap().young().remembered() = std::move(_remembered);
        Heap::bind_test_page_allocator(_allocator);
    }

private:
    RegionManager* _allocator;
    ZPageTable _pages;
    ZForwardingTable _young;
    ZForwardingTable _old;
    ZRemembered _remembered;
};

inline void BindFixtureRemembered(RegionManager& manager)
{
    auto& heap = Heap::GetHeap();
    const auto& map = Heap::page_table().map();
    const size_t size = map.size() * map.granule();
    heap.young().forwarding_table().initialize(size, map.base(), map.granule());
    heap.old().forwarding_table().initialize(size, map.base(), map.granule());
    heap.young().remembered()->bind(&Heap::page_table(), &heap.old().forwarding_table(), &manager);
}

inline void BindFixturePageTable(RegionManager& manager, size_t units)
{
    Heap::GetHeap().install_page_table(manager.GetRegionHeapStart(), units * ZPage::UNIT_SIZE, ZPage::UNIT_SIZE);
    Heap::bind_test_page_allocator(&manager);
    BindFixtureRemembered(manager);
}

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
      ZBackingIndexMax = static_cast<uint32_t>(ZBackingOffsetMax / ZBackingGranuleSize);

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
    const size_t maxCapacity = units * ZPage::UNIT_SIZE;
    _virtual.reset(new ZVirtualMemoryManager(maxCapacity));
    GC_EXPECT_TRUE(_virtual->is_initialized());
    _physical.reset(new ZPhysicalMemoryManager(maxCapacity));
    GC_EXPECT_TRUE(_physical->is_initialized());
    const std::vector<ZPage::UnitSegment> segments = RegionManager::ReservedSegments(*_virtual);
    _metadataSize = RegionManager::GetMetadataSize(ZPage::IndexedUnitCount(segments));
    _metadata = mmap(nullptr, _metadataSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    GC_EXPECT_TRUE(_metadata != MAP_FAILED);
    manager.Initialize(units, reinterpret_cast<uintptr_t>(_metadata), *_virtual, *_physical, params, garbageThreshold);
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
  ZFixtureRememberedScope _rememberedScope;
  ZTest::ZBackingLimitSetter _backingLimits;
  ZAddressOffsetMaxSetter _offsetMax;
  std::unique_ptr<ZVirtualMemoryManager> _virtual;
  std::unique_ptr<ZPhysicalMemoryManager> _physical;
  void* _metadata{ nullptr };
  size_t _metadataSize{ 0 };
};

} // namespace MapleRuntime
