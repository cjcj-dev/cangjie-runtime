// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zValue.inline.hpp:24-241
#pragma once
#include "Heap/z/zValue.hpp"

#include <cassert>
#include <new>

#include "Heap/z/workerThread.hpp"
#include "Heap/z/zCPU.inline.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zUtils.inline.hpp"

namespace MapleRuntime {
//
// Storage
//

template <typename T> uintptr_t ZValueStorage<T>::_end = 0;
template <typename T> uintptr_t ZValueStorage<T>::_top = 0;

template <typename S>
uintptr_t ZValueStorage<S>::alloc(size_t size)
{
    assert(size <= Offset && "Allocation too large");

    // Allocate entry in existing memory block
    const uintptr_t addr = (_top + S::alignment() - 1) & ~(S::alignment() - 1);
    _top = addr + size;

    if (_top < _end) {
        // Success
        return addr;
    }

    // Allocate new block of memory
    const size_t block_alignment = Offset;
    const size_t block_size = Offset * S::count();
    _top = ZUtils::alloc_aligned_unfreeable(block_alignment, block_size);
    _end = _top + Offset;

    // Retry allocation
    return alloc(size);
}

inline size_t ZContendedStorage::alignment()
{
    return ZCacheLineSize;
}

inline uint32_t ZContendedStorage::count()
{
    return 1;
}

inline uint32_t ZContendedStorage::id()
{
    return 0;
}

inline size_t ZPerCPUStorage::alignment()
{
    return sizeof(uintptr_t);
}

inline uint32_t ZPerCPUStorage::count()
{
    return ZCPU::count();
}

inline uint32_t ZPerCPUStorage::id()
{
    return ZCPU::id();
}

inline size_t ZPerNUMAStorage::alignment()
{
    return sizeof(uintptr_t);
}

// zValue.inline.hpp:84-90 read ZNUMA::count()/ZNUMA::id(). NUMA is deferred
// (A03n, user order); with NUMA disabled ZNUMA::count() is 1 and ZNUMA::id() is
// 0 (zNUMA.cpp), which is what these return until zNUMA lands.
inline uint32_t ZPerNUMAStorage::count()
{
    return 1;
}

inline uint32_t ZPerNUMAStorage::id()
{
    return 0;
}

inline size_t ZPerWorkerStorage::alignment()
{
    return sizeof(uintptr_t);
}

inline uint32_t ZPerWorkerStorage::count()
{
    return ConcGCThreads;
}

inline uint32_t ZPerWorkerStorage::id()
{
    return WorkerThread::worker_id();
}

//
// Value
//

template <typename S, typename T>
inline uintptr_t ZValue<S, T>::value_addr(uint32_t value_id) const
{
    return _addr + (value_id * S::Offset);
}

template <typename S, typename T>
inline ZValue<S, T>::ZValue()
    : _addr(S::alloc(sizeof(T)))
{
    // Initialize all instances
    ZValueIterator<S, T> iter(this);
    for (T* addr; iter.next(&addr);) {
        ::new (addr) T;
    }
}

template <typename S, typename T>
inline ZValue<S, T>::ZValue(const T& value)
    : _addr(S::alloc(sizeof(T)))
{
    // Initialize all instances
    ZValueIterator<S, T> iter(this);
    for (T* addr; iter.next(&addr);) {
        ::new (addr) T(value);
    }
}

template <typename S, typename T>
template <typename... Args>
inline ZValue<S, T>::ZValue(ZValueIdTagType, Args&&... args)
    : _addr(S::alloc(sizeof(T)))
{
    // Initialize all instances
    uint32_t value_id;
    ZValueIterator<S, T> iter(this);
    for (T* addr; iter.next(&addr, &value_id);) {
        ::new (addr) T(value_id, args...);
    }
}

template <typename S, typename T>
inline const T* ZValue<S, T>::addr(uint32_t value_id) const
{
    return reinterpret_cast<const T*>(value_addr(value_id));
}

template <typename S, typename T>
inline T* ZValue<S, T>::addr(uint32_t value_id)
{
    return reinterpret_cast<T*>(value_addr(value_id));
}

template <typename S, typename T>
inline const T& ZValue<S, T>::get(uint32_t value_id) const
{
    return *addr(value_id);
}

template <typename S, typename T>
inline T& ZValue<S, T>::get(uint32_t value_id)
{
    return *addr(value_id);
}

template <typename S, typename T>
inline void ZValue<S, T>::set(const T& value, uint32_t value_id)
{
    get(value_id) = value;
}

template <typename S, typename T>
inline void ZValue<S, T>::set_all(const T& value)
{
    ZValueIterator<S, T> iter(this);
    for (T* addr; iter.next(&addr);) {
        *addr = value;
    }
}

template <typename S, typename T>
uint32_t ZValue<S, T>::count() const
{
    return S::count();
}

//
// Iterator
//

template <typename S, typename T>
inline ZValueIterator<S, T>::ZValueIterator(ZValue<S, T>* value)
    : _value(value),
      _value_id(0) {}

template <typename S, typename T>
inline bool ZValueIterator<S, T>::next(T** value)
{
    if (_value_id < S::count()) {
        *value = _value->addr(_value_id++);
        return true;
    }
    return false;
}

template <typename S, typename T>
inline bool ZValueIterator<S, T>::next(T** value, uint32_t* value_id)
{
    if (_value_id < S::count()) {
        *value_id = _value_id;
        *value = _value->addr(_value_id++);
        return true;
    }
    return false;
}

template <typename S, typename T>
inline ZValueConstIterator<S, T>::ZValueConstIterator(const ZValue<S, T>* value)
    : _value(value),
      _value_id(0) {}

template <typename S, typename T>
inline ZValueConstIterator<S, T>::ZValueConstIterator(const ZValueIterator<S, T>& other)
    : _value(other._value),
      _value_id(other._value_id) {}

template <typename S, typename T>
inline bool ZValueConstIterator<S, T>::next(const T** value)
{
    if (_value_id < S::count()) {
        *value = _value->addr(_value_id++);
        return true;
    }
    return false;
}
} // namespace MapleRuntime
