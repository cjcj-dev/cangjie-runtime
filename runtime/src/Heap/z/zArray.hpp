// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zArray.hpp:24-128
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Heap/z/growableArray.hpp"
#include "Heap/z/zLock.hpp"

namespace MapleRuntime {
template<typename T> class ZArray;

template <typename T>
class ZArraySlice : public GrowableArrayView<T> {
    friend class ZArray<T>;
    friend class ZArray<std::remove_const_t<T>>;
    friend class ZArraySlice<std::remove_const_t<T>>;
    friend class ZArraySlice<const T>;

private:
    ZArraySlice(T* data, int len);

public:
    ZArraySlice<T> slice_front(int end);
    ZArraySlice<const T> slice_front(int end) const;

    ZArraySlice<T> slice_back(int start);
    ZArraySlice<const T> slice_back(int start) const;

    ZArraySlice<T> slice(int start, int end);
    ZArraySlice<const T> slice(int start, int end) const;

    operator ZArraySlice<const T>() const;
};

template <typename T>
class ZArray : public GrowableArrayCHeap<T> {
public:
    using GrowableArrayCHeap<T>::GrowableArrayCHeap;

    ZArraySlice<T> slice_front(int end);
    ZArraySlice<const T> slice_front(int end) const;

    ZArraySlice<T> slice_back(int start);
    ZArraySlice<const T> slice_back(int start) const;

    ZArraySlice<T> slice(int start, int end);
    ZArraySlice<const T> slice(int start, int end) const;

    operator ZArraySlice<T>();
    operator ZArraySlice<const T>() const;
};

template <typename T, bool Parallel>
class ZArrayIteratorImpl {
private:
    using NextType = std::conditional_t<Parallel, std::atomic<size_t>, size_t>;

    NextType       _next;
    const size_t   _end;
    const T* const _array;

    bool next_serial(size_t* index);
    bool next_parallel(size_t* index);

    // C++14 has no `if constexpr` (zArray.inline.hpp:171-177): the Parallel
    // branch point is a tag-dispatched overload pair instead.
    bool next_index(size_t* index, std::true_type parallel);
    bool next_index(size_t* index, std::false_type serial);

public:
    ZArrayIteratorImpl(const T* array, size_t length);
    ZArrayIteratorImpl(const ZArray<T>* array);

    bool next(T* elem);

    template <typename Function, typename... Args>
    bool next_if(T* elem, Function predicate, Args&&... args);

    bool next_index(size_t* index);

    T index_to_elem(size_t index);
};

template <typename T> using ZArrayIterator = ZArrayIteratorImpl<T, false /* Parallel */>;
template <typename T> using ZArrayParallelIterator = ZArrayIteratorImpl<T, true /* Parallel */>;

template <typename T>
class ZActivatedArray {
private:
    typedef typename std::remove_extent<T>::type ItemT;

    ZLock*         _lock;
    uint64_t       _count;
    ZArray<ItemT*> _array;

public:
    explicit ZActivatedArray(bool locked = true);
    ~ZActivatedArray();

    void activate();
    template <typename Function>
    void deactivate_and_apply(Function function);

    bool is_activated() const;
    bool add_if_activated(ItemT* item);
};
} // namespace MapleRuntime
