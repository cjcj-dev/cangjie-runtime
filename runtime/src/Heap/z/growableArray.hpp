// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// utilities/growableArray.hpp — the HotSpot container ZArray is a thin shell
// over (zArray.hpp:37-76). The same class decomposition is kept:
// GrowableArrayBase (:73-95) → GrowableArrayView<E> (:112-316) →
// GrowableArrayWithAllocator<E, Derived> (:326-534) → GrowableArrayCHeap<E>
// (:837-877); only the members gc/z consumes are ported. The element model is
// HotSpot's: allocate() hands back raw storage, the constructors and
// expand_to/shrink_to_fit construct every one of the `_capacity` slots
// (:333-346, :538-551, :566-593), the mutators assign into constructed slots
// (:355-359, :376-407, :469-528), and clear_and_deallocate destroys all
// `_capacity` slots (:587, :596-599). No MemTag / Arena / ResourceArea halves:
// the C heap is the only backing store here.
#pragma once
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

#include "Base/Globals.h"

namespace MapleRuntime {
// utilities/growableArray.hpp:73-95. Non-template base class responsible for
// handling the length and max.
class GrowableArrayBase {
protected:
    // Current number of accessible elements
    int _len;
    // Current number of allocated elements
    int _capacity;

    GrowableArrayBase(int capacity, int initial_len)
        : _len(initial_len), _capacity(capacity)
    {
        assert(_len >= 0 && _len <= _capacity);
    }

    ~GrowableArrayBase() {}

public:
    int length() const { return _len; }
    int capacity() const { return _capacity; }

    bool is_empty() const { return _len == 0; }
    bool is_nonempty() const { return _len != 0; }
    bool is_full() const { return _len == _capacity; }
};

// utilities/growableArray.hpp:112-316. Extends GrowableArrayBase with a typed
// data array. The "view" adds function that don't grow or deallocate the
// _data array, so there's no need for an allocator.
template <typename E>
class GrowableArrayView : public GrowableArrayBase {
protected:
    E* _data;

    GrowableArrayView(E* data, int capacity, int initial_len)
        : GrowableArrayBase(capacity, initial_len), _data(data) {}

    ~GrowableArrayView() {}

public:
    bool operator==(const GrowableArrayView& rhs) const
    {
        if (_len != rhs._len) {
            return false;
        }
        for (int i = 0; i < _len; i++) {
            if (!(at(i) == rhs.at(i))) {
                return false;
            }
        }
        return true;
    }

    bool operator!=(const GrowableArrayView& rhs) const { return !(*this == rhs); }

    E& at(int i)
    {
        assert(0 <= i && i < _len);
        return _data[i];
    }

    E const& at(int i) const
    {
        assert(0 <= i && i < _len);
        return _data[i];
    }

    E* adr_at(int i) const
    {
        assert(0 <= i && i < _len);
        return &_data[i];
    }

    E& first()
    {
        assert(_len > 0);
        return _data[0];
    }

    E const& first() const
    {
        assert(_len > 0);
        return _data[0];
    }

    E& top()
    {
        assert(_len > 0);
        return _data[_len - 1];
    }

    E const& top() const
    {
        assert(_len > 0);
        return _data[_len - 1];
    }

    E& last() { return top(); }

    E const& last() const { return top(); }

    // GrowableArrayIterator<E> (:880-916) is a pointer walk over _data; the
    // raw pointer is that iterator.
    E* begin() const { return _data; }
    E* end() const { return _data + _len; }

    void at_put(int i, const E& elem)
    {
        assert(0 <= i && i < _len);
        _data[i] = elem;
    }

    bool contains(const E& elem) const { return find(elem) >= 0; }

    int find(const E& elem) const
    {
        for (int i = 0; i < _len; i++) {
            if (_data[i] == elem) {
                return i;
            }
        }
        return -1;
    }

    int find_from_end(const E& elem) const
    {
        for (int i = _len - 1; i >= 0; i--) {
            if (_data[i] == elem) {
                return i;
            }
        }
        return -1;
    }

    template <typename Predicate>
    int find_if(Predicate predicate) const
    {
        for (int i = 0; i < _len; i++) {
            if (predicate(_data[i])) {
                return i;
            }
        }
        return -1;
    }

    template <typename Predicate>
    int find_from_end_if(Predicate predicate) const
    {
        for (int i = _len - 1; i >= 0; i--) {
            if (predicate(_data[i])) {
                return i;
            }
        }
        return -1;
    }

    void sort(int f(E*, E*))
    {
        if (_data == nullptr) {
            return;
        }
        std::qsort(_data, static_cast<size_t>(_len), sizeof(E),
                   reinterpret_cast<int (*)(const void*, const void*)>(f));
    }

    E** data_addr() { return &_data; }
};

// utilities/growableArray.hpp:318-534. GrowableArrayWithAllocator extends the
// "view" with the capability to grow and deallocate the data array.
//
// The allocator responsibility is delegated to the sub-class.
//
// Derived: The sub-class responsible for allocation / deallocation
//  - E* Derived::allocate()       - member function responsible for allocation
//  - void Derived::deallocate(E*) - member function responsible for deallocation
template <typename E, typename Derived>
class GrowableArrayWithAllocator : public GrowableArrayView<E> {
    void expand_to(int j);
    void grow(int j);

protected:
    GrowableArrayWithAllocator(E* data, int capacity)
        : GrowableArrayView<E>(data, capacity, 0)
    {
        for (int i = 0; i < capacity; i++) {
            ::new (static_cast<void*>(&data[i])) E();
        }
    }

    GrowableArrayWithAllocator(E* data, int capacity, int initial_len, const E& filler)
        : GrowableArrayView<E>(data, capacity, initial_len)
    {
        int i = 0;
        for (; i < initial_len; i++) {
            ::new (static_cast<void*>(&data[i])) E(filler);
        }
        for (; i < capacity; i++) {
            ::new (static_cast<void*>(&data[i])) E();
        }
    }

    GrowableArrayWithAllocator(E* data, int capacity, int initial_len)
        : GrowableArrayView<E>(data, capacity, initial_len) {}

    ~GrowableArrayWithAllocator() {}

public:
    int append(const E& elem)
    {
        if (this->_len == this->_capacity) {
            grow(this->_len);
        }
        int idx = this->_len++;
        this->_data[idx] = elem;
        return idx;
    }

    bool append_if_missing(const E& elem)
    {
        // Returns TRUE if elem is added.
        bool missed = !this->contains(elem);
        if (missed) {
            append(elem);
        }
        return missed;
    }

    void push(const E& elem) { append(elem); }

    E pop()
    {
        assert(this->_len > 0);
        return this->_data[--this->_len];
    }

    E& at_grow(int i, const E& fill = E())
    {
        assert(0 <= i);
        if (i >= this->_len) {
            if (i >= this->_capacity) {
                grow(i);
            }
            for (int j = this->_len; j <= i; j++) {
                this->_data[j] = fill;
            }
            this->_len = i + 1;
        }
        return this->_data[i];
    }

    void at_put_grow(int i, const E& elem, const E& fill = E())
    {
        assert(0 <= i);
        if (i >= this->_len) {
            if (i >= this->_capacity) {
                grow(i);
            }
            for (int j = this->_len; j < i; j++) {
                this->_data[j] = fill;
            }
            this->_len = i + 1;
        }
        this->_data[i] = elem;
    }

    // inserts the given element before the element at index i
    void insert_before(const int idx, const E& elem)
    {
        assert(0 <= idx && idx <= this->_len);
        if (this->_len == this->_capacity) {
            grow(this->_len);
        }
        for (int j = this->_len - 1; j >= idx; j--) {
            this->_data[j + 1] = this->_data[j];
        }
        this->_len++;
        this->_data[idx] = elem;
    }

    void insert_before(const int idx, const GrowableArrayView<E>* array)
    {
        assert(0 <= idx && idx <= this->_len);
        int array_len = array->length();
        int new_len = this->_len + array_len;
        if (new_len >= this->_capacity) {
            grow(new_len);
        }

        for (int j = this->_len - 1; j >= idx; j--) {
            this->_data[j + array_len] = this->_data[j];
        }

        for (int j = 0; j < array_len; j++) {
            this->_data[idx + j] = array->at(j);
        }

        this->_len += array_len;
    }

    void appendAll(const GrowableArrayView<E>* l)
    {
        for (int i = 0; i < l->length(); i++) {
            this->at_put_grow(this->_len, l->at(i), E());
        }
    }

    void swap(GrowableArrayWithAllocator* other)
    {
        std::swap(this->_data, other->_data);
        std::swap(this->_len, other->_len);
        std::swap(this->_capacity, other->_capacity);
    }

    // Ensure capacity is at least new_capacity.
    void reserve(int new_capacity);

    void trunc_to(int length)
    {
        assert(length <= this->_len);
        this->_len = length;
    }

    // Order preserving remove operations.

    void remove_at(int index)
    {
        assert(0 <= index && index < this->_len);
        for (int j = index + 1; j < this->_len; j++) {
            this->_data[j - 1] = this->_data[j];
        }
        this->_len--;
    }

    void remove(const E& elem)
    {
        // Assuming that element does exist.
        bool removed = this->remove_if_existing(elem);
        if (removed) {
            return;
        }
        assert(false && "ShouldNotReachHere");
        std::abort();
    }

    bool remove_if_existing(const E& elem)
    {
        // Returns TRUE if elem is removed.
        for (int i = 0; i < this->_len; i++) {
            if (this->_data[i] == elem) {
                this->remove_at(i);
                return true;
            }
        }
        return false;
    }

    // Remove all elements in the range [0; end). The order is preserved.
    void remove_till(int end) { remove_range(0, end); }

    // Remove all elements in the range [start; end). The order is preserved.
    void remove_range(int start, int end)
    {
        assert(0 <= start);
        assert(start <= end && end <= this->_len);

        for (int i = start, j = end; j < this->length(); i++, j++) {
            this->at_put(i, this->at(j));
        }
        this->_len -= (end - start);
    }

    // Replaces the designated element with the last element and shrinks by 1.
    void delete_at(int index)
    {
        assert(0 <= index && index < this->_len);
        if (index < --this->_len) {
            // Replace removed element with last one.
            this->_data[index] = this->_data[this->_len];
        }
    }

    // Reduce capacity to length.
    void shrink_to_fit();

    void clear() { this->_len = 0; }
    void clear_and_deallocate();
};

template <typename E, typename Derived>
void GrowableArrayWithAllocator<E, Derived>::expand_to(int new_capacity)
{
    int old_capacity = this->_capacity;
    assert(new_capacity > old_capacity);
    this->_capacity = new_capacity;
    E* newData = static_cast<Derived*>(this)->allocate();
    int i = 0;
    for (; i < this->_len; i++) {
        ::new (static_cast<void*>(&newData[i])) E(this->_data[i]);
    }
    for (; i < this->_capacity; i++) {
        ::new (static_cast<void*>(&newData[i])) E();
    }
    for (i = 0; i < old_capacity; i++) {
        this->_data[i].~E();
    }
    if (this->_data != nullptr) {
        static_cast<Derived*>(this)->deallocate(this->_data);
    }
    this->_data = newData;
}

template <typename E, typename Derived>
void GrowableArrayWithAllocator<E, Derived>::grow(int j)
{
    // grow the array by increasing _capacity to the first power of two larger than the size we need
    expand_to(NextPowerOfTwo(j));
}

template <typename E, typename Derived>
void GrowableArrayWithAllocator<E, Derived>::reserve(int new_capacity)
{
    if (new_capacity > this->_capacity) {
        expand_to(new_capacity);
    }
}

template <typename E, typename Derived>
void GrowableArrayWithAllocator<E, Derived>::shrink_to_fit()
{
    int old_capacity = this->_capacity;
    int len = this->_len;
    assert(len <= old_capacity);

    // If already at full capacity, nothing to do.
    if (len == old_capacity) {
        return;
    }

    // If not empty, allocate new, smaller, data, and copy old data to it.
    E* old_data = this->_data;
    E* new_data = nullptr;
    this->_capacity = len; // Must preceed allocate().
    if (len > 0) {
        new_data = static_cast<Derived*>(this)->allocate();
        for (int i = 0; i < len; ++i) {
            ::new (static_cast<void*>(&new_data[i])) E(old_data[i]);
        }
    }
    // Destroy contents of old data, and deallocate it.
    for (int i = 0; i < old_capacity; ++i) {
        old_data[i].~E();
    }
    if (old_data != nullptr) {
        static_cast<Derived*>(this)->deallocate(old_data);
    }
    // Install new data, which might be nullptr.
    this->_data = new_data;
}

template <typename E, typename Derived>
void GrowableArrayWithAllocator<E, Derived>::clear_and_deallocate()
{
    this->clear();
    this->shrink_to_fit();
}

// utilities/growableArray.hpp:613-617, growableArray.cpp:45-63. CHeap
// allocator: raw storage for `max` elements, nullptr when max == 0.
class GrowableArrayCHeapAllocator {
public:
    static void* allocate(int max, int element_size)
    {
        assert(max >= 0);
        if (max == 0) {
            return nullptr;
        }
        size_t byte_size = static_cast<size_t>(element_size) * static_cast<size_t>(max);
        void* const mem = std::malloc(byte_size);
        if (mem == nullptr) {
            std::abort();
        }
        return mem;
    }

    static void deallocate(void* mem) { std::free(mem); }
};

// utilities/growableArray.hpp:837-877. Leaner GrowableArray for CHeap backed
// data arrays (the MemTag parameter has no counterpart here).
template <typename E>
class GrowableArrayCHeap : public GrowableArrayWithAllocator<E, GrowableArrayCHeap<E>> {
    friend class GrowableArrayWithAllocator<E, GrowableArrayCHeap<E>>;

    static E* allocate(int max)
    {
        return static_cast<E*>(GrowableArrayCHeapAllocator::allocate(max, sizeof(E)));
    }

    E* allocate() { return allocate(this->_capacity); }

    void deallocate(E* mem) { GrowableArrayCHeapAllocator::deallocate(mem); }

public:
    GrowableArrayCHeap(int initial_capacity = 0)
        : GrowableArrayWithAllocator<E, GrowableArrayCHeap<E>>(allocate(initial_capacity), initial_capacity) {}

    GrowableArrayCHeap(int initial_capacity, int initial_len, const E& filler)
        : GrowableArrayWithAllocator<E, GrowableArrayCHeap<E>>(allocate(initial_capacity), initial_capacity,
                                                                 initial_len, filler) {}

    GrowableArrayCHeap(const GrowableArrayCHeap&) = delete;
    GrowableArrayCHeap& operator=(const GrowableArrayCHeap&) = delete;

    ~GrowableArrayCHeap() { this->clear_and_deallocate(); }
};
} // namespace MapleRuntime
