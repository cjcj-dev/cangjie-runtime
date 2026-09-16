// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/windows/gc/z/zPhysicalMemoryBacking_windows.cpp:34-260.

#include "Heap/z/zPhysicalMemoryBacking_windows.hpp"

#include <cassert>
#include <vector>

#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zLargePages.inline.hpp"
#include "Heap/z/zMapper_windows.hpp"

namespace MapleRuntime {

class ZPhysicalMemoryBackingImpl {
public:
  virtual ~ZPhysicalMemoryBackingImpl() = default;
  virtual size_t commit(zbacking_offset offset, size_t size) = 0;
  virtual size_t uncommit(zbacking_offset offset, size_t size) = 0;
  virtual void map(zaddress_unsafe addr, size_t size, zbacking_offset offset) const = 0;
  virtual void unmap(zaddress_unsafe addr, size_t size) const = 0;
};

class ZPhysicalMemoryBackingSmallPages : public ZPhysicalMemoryBackingImpl {
private:
  std::vector<HANDLE> _handles;

  size_t index_of(zbacking_offset offset) const {
    return untype(offset) / ZGranuleSize;
  }

public:
  explicit ZPhysicalMemoryBackingSmallPages(size_t max_capacity)
    : _handles(max_capacity / ZGranuleSize, 0) {}

  size_t commit(zbacking_offset offset, size_t size) override {
    for (size_t i = 0; i < size; i += ZGranuleSize) {
      HANDLE const handle = ZMapper::create_and_commit_paging_file_mapping(ZGranuleSize);
      if (handle == 0) {
        return i;
      }
      _handles[index_of(to_zbacking_offset(untype(offset) + i))] = handle;
    }
    return size;
  }

  size_t uncommit(zbacking_offset offset, size_t size) override {
    for (size_t i = 0; i < size; i += ZGranuleSize) {
      const size_t idx = index_of(to_zbacking_offset(untype(offset) + i));
      HANDLE const handle = _handles[idx];
      _handles[idx] = 0;
      ZMapper::close_paging_file_mapping(handle);
    }
    return size;
  }

  void map(zaddress_unsafe addr, size_t size, zbacking_offset offset) const override {
    for (size_t i = 0; i < size; i += ZGranuleSize) {
      HANDLE const handle = _handles[index_of(to_zbacking_offset(untype(offset) + i))];
      ZMapper::map_view_replace_placeholder(
        handle, 0, to_zaddress_unsafe(untype(addr) + i), ZGranuleSize);
    }
  }

  void unmap(zaddress_unsafe addr, size_t size) const override {
    for (size_t i = 0; i < size; i += ZGranuleSize) {
      ZMapper::unmap_view_preserve_placeholder(to_zaddress_unsafe(untype(addr) + i), ZGranuleSize);
    }
  }
};

HANDLE ZAWESection;

class ZPhysicalMemoryBackingLargePages : public ZPhysicalMemoryBackingImpl {
private:
  std::vector<ULONG_PTR> _page_array;

public:
  explicit ZPhysicalMemoryBackingLargePages(size_t max_capacity)
    : _page_array(max_capacity / ZGranuleSize, 0) {}

  size_t commit(zbacking_offset offset, size_t size) override {
    const size_t index = untype(offset) >> ZGranuleSizeShift;
    size_t npages = size >> ZGranuleSizeShift;
    const BOOL res = AllocateUserPhysicalPages(ZAWESection, &npages, &_page_array[index]);
    (void)res;
    return npages << ZGranuleSizeShift;
  }

  size_t uncommit(zbacking_offset offset, size_t size) override {
    const size_t index = untype(offset) >> ZGranuleSizeShift;
    size_t npages = size >> ZGranuleSizeShift;
    FreeUserPhysicalPages(ZAWESection, &npages, &_page_array[index]);
    return npages << ZGranuleSizeShift;
  }

  void map(zaddress_unsafe addr, size_t size, zbacking_offset offset) const override {
    const size_t npages = size >> ZGranuleSizeShift;
    const size_t index = untype(offset) >> ZGranuleSizeShift;
    MapUserPhysicalPages(reinterpret_cast<void*>(untype(addr)), npages,
                         const_cast<ULONG_PTR*>(&_page_array[index]));
  }

  void unmap(zaddress_unsafe addr, size_t size) const override {
    const size_t npages = size >> ZGranuleSizeShift;
    MapUserPhysicalPages(reinterpret_cast<void*>(untype(addr)), npages, nullptr);
  }
};

static ZPhysicalMemoryBackingImpl* select_impl(size_t max_capacity) {
  if (ZLargePages::is_enabled()) {
    return new ZPhysicalMemoryBackingLargePages(max_capacity);
  }
  return new ZPhysicalMemoryBackingSmallPages(max_capacity);
}

ZPhysicalMemoryBacking::ZPhysicalMemoryBacking(size_t max_capacity)
  : _impl(select_impl(max_capacity)) {}

bool ZPhysicalMemoryBacking::is_initialized() const {
  return true;
}

void ZPhysicalMemoryBacking::warn_commit_limits(size_t max_capacity) const {
  (void)max_capacity;
}

size_t ZPhysicalMemoryBacking::commit(zbacking_offset offset, size_t length, uint32_t) {
  return _impl->commit(offset, length);
}

size_t ZPhysicalMemoryBacking::uncommit(zbacking_offset offset, size_t length) {
  return _impl->uncommit(offset, length);
}

void ZPhysicalMemoryBacking::map(zaddress_unsafe addr, size_t size, zbacking_offset offset) const {
  _impl->map(addr, size, offset);
}

void ZPhysicalMemoryBacking::unmap(zaddress_unsafe addr, size_t size) const {
  _impl->unmap(addr, size);
}

}
