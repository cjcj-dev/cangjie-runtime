// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Placeholder for ZGC zValue.hpp:35-156 (P06 replaces this file with the
// ZValueStorage/ZValue family). Only ZPerNUMA<T> and its iterator are
// provided here because the memory managers keep one registry per NUMA
// partition (zVirtualMemoryManager.hpp:79, zPhysicalMemoryManager.hpp:45).
// The partition count comes from the sealed process topology until A03n
// lands the static ZNUMA class (ZNUMA::count(), zNUMA.inline.hpp:33-40).

#pragma once
#include <cassert>
#include <cstdint>
#include <new>

#include "Heap/z/zNUMA.inline.hpp"

namespace MapleRuntime {

template <typename T>
class ZPerNUMA {
private:
  const uint32_t _count;
  T* const       _values;

  ZPerNUMA(const ZPerNUMA&) = delete;
  ZPerNUMA& operator=(const ZPerNUMA&) = delete;

  static uint32_t numa_count() {
    return static_cast<uint32_t>(NumaTopology::SealProcessTopology().Count());
  }

public:
  ZPerNUMA()
    : _count(numa_count()),
      _values(static_cast<T*>(::operator new(sizeof(T) * _count))) {
    for (uint32_t id = 0; id < _count; id++) {
      ::new (_values + id) T();
    }
  }

  ~ZPerNUMA() {
    for (uint32_t id = 0; id < _count; id++) {
      _values[id].~T();
    }
    ::operator delete(_values);
  }

  uint32_t count() const {
    return _count;
  }

  const T& get(uint32_t id) const {
    assert(id < _count);
    return _values[id];
  }

  T& get(uint32_t id) {
    assert(id < _count);
    return _values[id];
  }

  T* addr(uint32_t id) {
    assert(id < _count);
    return _values + id;
  }
};

template <typename T>
class ZPerNUMAIterator {
private:
  ZPerNUMA<T>* const _value;
  uint32_t           _value_id;

public:
  ZPerNUMAIterator(ZPerNUMA<T>* value)
    : _value(value),
      _value_id(0) {}

  bool next(T** value, uint32_t* value_id) {
    if (_value_id < _value->count()) {
      *value = _value->addr(_value_id);
      *value_id = _value_id++;
      return true;
    }
    return false;
  }
};

} // namespace MapleRuntime
