// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zArray.inline.hpp:24-247
#pragma once
#include "Heap/z/zArray.hpp"

#include <cassert>

#include "Heap/z/zLock.inline.hpp"

namespace MapleRuntime {
template <typename T>
ZArraySlice<T>::ZArraySlice(T* data, int len)
    : GrowableArrayView<T>(data, len, len) {}

template <typename T>
ZArraySlice<T> ZArraySlice<T>::slice_front(int end)
{
    return slice(0, end);
}

template <typename T>
ZArraySlice<const T> ZArraySlice<T>::slice_front(int end) const
{
    return slice(0, end);
}

template <typename T>
ZArraySlice<T> ZArraySlice<T>::slice_back(int start)
{
    return slice(start, this->_len);
}

template <typename T>
ZArraySlice<const T> ZArraySlice<T>::slice_back(int start) const
{
    return slice(start, this->_len);
}

template <typename T>
ZArraySlice<T> ZArraySlice<T>::slice(int start, int end)
{
    assert(0 <= start && start <= end && end <= this->_len);
    return ZArraySlice<T>(this->_data + start, end - start);
}

template <typename T>
ZArraySlice<const T> ZArraySlice<T>::slice(int start, int end) const
{
    assert(0 <= start && start <= end && end <= this->_len);
    return ZArraySlice<const T>(this->_data + start, end - start);
}

template <typename T>
ZArraySlice<T>::operator ZArraySlice<const T>() const
{
    return slice(0, this->_len);
}

template <typename T>
ZArraySlice<T> ZArray<T>::slice_front(int end)
{
    return slice(0, end);
}

template <typename T>
ZArraySlice<const T> ZArray<T>::slice_front(int end) const
{
    return slice(0, end);
}

template <typename T>
ZArraySlice<T> ZArray<T>::slice_back(int start)
{
    return slice(start, this->_len);
}

template <typename T>
ZArraySlice<const T> ZArray<T>::slice_back(int start) const
{
    return slice(start, this->_len);
}

template <typename T>
ZArraySlice<T> ZArray<T>::slice(int start, int end)
{
    assert(0 <= start && start <= end && end <= this->_len);
    return ZArraySlice<T>(this->_data + start, end - start);
}

template <typename T>
ZArraySlice<const T> ZArray<T>::slice(int start, int end) const
{
    assert(0 <= start && start <= end && end <= this->_len);
    return ZArraySlice<const T>(this->_data + start, end - start);
}

template <typename T>
ZArray<T>::operator ZArraySlice<T>()
{
    return slice(0, this->_len);
}

template <typename T>
ZArray<T>::operator ZArraySlice<const T>() const
{
    return slice(0, this->_len);
}

template <typename T, bool Parallel>
inline bool ZArrayIteratorImpl<T, Parallel>::next_serial(size_t* index)
{
    if (_next == _end) {
        return false;
    }

    *index = _next;
    _next++;

    return true;
}

template <typename T, bool Parallel>
inline bool ZArrayIteratorImpl<T, Parallel>::next_parallel(size_t* index)
{
    const size_t claimed_index = _next.fetch_add(1u, std::memory_order_relaxed);

    if (claimed_index < _end) {
        *index = claimed_index;
        return true;
    }

    return false;
}

template <typename T, bool Parallel>
inline ZArrayIteratorImpl<T, Parallel>::ZArrayIteratorImpl(const T* array, size_t length)
    : _next(0),
      _end(length),
      _array(array) {}

template <typename T, bool Parallel>
inline ZArrayIteratorImpl<T, Parallel>::ZArrayIteratorImpl(const ZArray<T>* array)
    : ZArrayIteratorImpl<T, Parallel>(array->is_empty() ? nullptr : array->adr_at(0), (size_t)array->length()) {}

template <typename T, bool Parallel>
inline bool ZArrayIteratorImpl<T, Parallel>::next(T* elem)
{
    size_t index;
    if (next_index(&index)) {
        *elem = index_to_elem(index);
        return true;
    }

    return false;
}

template <typename T, bool Parallel>
template <typename Function, typename... Args>
inline bool ZArrayIteratorImpl<T, Parallel>::next_if(T* elem, Function predicate, Args&&... args)
{
    size_t index;
    while (next_index(&index)) {
        if (predicate(index_to_elem(index), args...)) {
            *elem = index_to_elem(index);
            return true;
        }
    }

    return false;
}

template <typename T, bool Parallel>
inline bool ZArrayIteratorImpl<T, Parallel>::next_index(size_t* index, std::true_type)
{
    return next_parallel(index);
}

template <typename T, bool Parallel>
inline bool ZArrayIteratorImpl<T, Parallel>::next_index(size_t* index, std::false_type)
{
    return next_serial(index);
}

template <typename T, bool Parallel>
inline bool ZArrayIteratorImpl<T, Parallel>::next_index(size_t* index)
{
    return next_index(index, std::integral_constant<bool, Parallel>());
}

template <typename T, bool Parallel>
inline T ZArrayIteratorImpl<T, Parallel>::index_to_elem(size_t index)
{
    assert(index < _end && "Out of bounds");
    return _array[index];
}

template <typename T>
ZActivatedArray<T>::ZActivatedArray(bool locked)
    : _lock(locked ? new ZLock() : nullptr),
      _count(0),
      _array() {}

template <typename T>
ZActivatedArray<T>::~ZActivatedArray()
{
    delete _lock;
}

template <typename T>
bool ZActivatedArray<T>::is_activated() const
{
    ZLocker<ZLock> locker(_lock);
    return _count > 0;
}

template <typename T>
bool ZActivatedArray<T>::add_if_activated(ItemT* item)
{
    ZLocker<ZLock> locker(_lock);
    if (_count > 0) {
        _array.append(item);
        return true;
    }

    return false;
}

template <typename T>
void ZActivatedArray<T>::activate()
{
    ZLocker<ZLock> locker(_lock);
    _count++;
}

template <typename T>
template <typename Function>
void ZActivatedArray<T>::deactivate_and_apply(Function function)
{
    ZArray<ItemT*> array;

    {
        ZLocker<ZLock> locker(_lock);
        assert(_count > 0 && "Invalid state");
        if (--_count == 0u) {
            // Fully deactivated - remove all elements
            array.swap(&_array);
        }
    }

    // Apply function to all elements - if fully deactivated
    ZArrayIterator<ItemT*> iter(&array);
    for (ItemT* item; iter.next(&item);) {
        function(item);
    }
}
} // namespace MapleRuntime
