// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/windows/gc/z/zVirtualMemoryManager_windows.cpp:35-229.

#include "Heap/z/zVirtualMemoryManager.hpp"

#include <cassert>

#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zLargePages.inline.hpp"
#include "Heap/z/zMapper_windows.hpp"
#include "Heap/z/zRange.inline.hpp"
#include "Heap/z/zRangeRegistry.inline.hpp"
#include "Heap/z/zSyscall_windows.hpp"
#include "Heap/z/zVirtualMemory.inline.hpp"

namespace MapleRuntime {

class ZVirtualMemoryReserverImpl {
public:
  virtual ~ZVirtualMemoryReserverImpl() = default;
  virtual void register_callbacks(ZVirtualMemoryRegistry* registry) { (void)registry; }
  virtual bool reserve(zaddress_unsafe addr, size_t size) = 0;
  virtual void unreserve(zaddress_unsafe addr, size_t size) = 0;
};

class ZVirtualMemoryReserverSmallPages : public ZVirtualMemoryReserverImpl {
private:
  class PlaceholderCallbacks {
  private:
    static void split_placeholder(zoffset start, size_t size) {
      ZMapper::split_placeholder(ZOffset::address_unsafe(start), size);
    }

    static void coalesce_placeholders(zoffset start, size_t size) {
      ZMapper::coalesce_placeholders(ZOffset::address_unsafe(start), size);
    }

    static void split_into_granule_sized_placeholders(zoffset start, size_t size) {
      assert(size >= ZGranuleSize);
      const size_t limit = size - ZGranuleSize;
      for (size_t offset = 0; offset < limit; offset += ZGranuleSize) {
        split_placeholder(to_zoffset(untype(start) + offset), ZGranuleSize);
      }
    }

    static void coalesce_into_one_placeholder(zoffset start, size_t size) {
      if (size > ZGranuleSize) {
        coalesce_placeholders(start, size);
      }
    }

    static void prepare_for_hand_out_callback(const ZVirtualMemory& area) {
      split_into_granule_sized_placeholders(area.start(), area.size());
    }

    static void prepare_for_hand_back_callback(const ZVirtualMemory& area) {
      coalesce_into_one_placeholder(area.start(), area.size());
    }

    static void grow_callback(const ZVirtualMemory& from, const ZVirtualMemory& to) {
      (void)from;
      coalesce_into_one_placeholder(to.start(), to.size());
    }

    static void shrink_callback(const ZVirtualMemory& from, const ZVirtualMemory& to) {
      (void)from;
      split_placeholder(to.start(), to.size());
    }

  public:
    static ZVirtualMemoryRegistry::Callbacks callbacks() {
      ZVirtualMemoryRegistry::Callbacks callbacks;
      callbacks._prepare_for_hand_out = &prepare_for_hand_out_callback;
      callbacks._prepare_for_hand_back = &prepare_for_hand_back_callback;
      callbacks._grow = &grow_callback;
      callbacks._shrink = &shrink_callback;
      return callbacks;
    }
  };

  void register_callbacks(ZVirtualMemoryRegistry* registry) override {
    registry->register_callbacks(PlaceholderCallbacks::callbacks());
  }

  bool reserve(zaddress_unsafe addr, size_t size) override {
    const zaddress_unsafe res = ZMapper::reserve(addr, size);
    return res == addr;
  }

  void unreserve(zaddress_unsafe addr, size_t size) override {
    ZMapper::unreserve(addr, size);
  }
};

extern HANDLE ZAWESection;

class ZVirtualMemoryReserverLargePages : public ZVirtualMemoryReserverImpl {
private:
  bool reserve(zaddress_unsafe addr, size_t size) override {
    const zaddress_unsafe res = ZMapper::reserve_for_shared_awe(ZAWESection, addr, size);
    return res == addr;
  }

  void unreserve(zaddress_unsafe addr, size_t size) override {
    ZMapper::unreserve_for_shared_awe(addr, size);
  }

public:
  ZVirtualMemoryReserverLargePages() {
    ZAWESection = ZMapper::create_shared_awe_section();
  }
};

static ZVirtualMemoryReserverImpl* _impl = nullptr;

static void ZVirtualMemoryReserverImpl_initialize() {
  if (_impl != nullptr) {
    return;
  }
  ZSyscall::initialize();
  if (ZLargePages::is_enabled()) {
    _impl = new ZVirtualMemoryReserverLargePages();
  } else {
    _impl = new ZVirtualMemoryReserverSmallPages();
  }
}

void ZVirtualMemoryReserver::pd_register_callbacks(ZVirtualMemoryRegistry* registry) {
  ZVirtualMemoryReserverImpl_initialize();
  _impl->register_callbacks(registry);
}

bool ZVirtualMemoryReserver::pd_reserve(zaddress_unsafe addr, size_t size) {
  ZVirtualMemoryReserverImpl_initialize();
  return _impl->reserve(addr, size);
}

void ZVirtualMemoryReserver::pd_unreserve(zaddress_unsafe addr, size_t size) {
  ZVirtualMemoryReserverImpl_initialize();
  _impl->unreserve(addr, size);
}

}
