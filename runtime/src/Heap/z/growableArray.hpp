// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// utilities/growableArray.hpp — the HotSpot container ZArray is a thin shell
// over (zArray.hpp:37-76). Only the view/CHeap halves that gc/z consumes are
// here: {_data, _len, _capacity} data form, append/at/adr_at/first/last/
// clear/trunc_to/remove_at/delete_at/insert_before/at_grow/swap/appendAll.
#pragma once
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace MapleRuntime {
template <typename E>
class GrowableArrayView {
protected:
    E*  _data;
    int _len;
    int _capacity;

    GrowableArrayView(E* data, int capacity, int initial_len)
        : _data(data), _len(initial_len), _capacity(capacity) {}

public:
    int length() const { return _len; }
    int capacity() const { return _capacity; }

    bool is_empty() const { return _len == 0; }
    bool is_nonempty() const { return _len != 0; }

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

    E first() const
    {
        assert(_len > 0);
        return _data[0];
    }

    E top() const
    {
        assert(_len > 0);
        return _data[_len - 1];
    }

    E last() const { return top(); }

    E* begin() const { return _data; }
    E* end() const { return _data + _len; }

    void at_put(int i, const E& elem)
    {
        assert(0 <= i && i < _len);
        _data[i] = elem;
    }

    E pop()
    {
        assert(_len > 0);
        return _data[--_len];
    }

    void clear() { _len = 0; }

    void trunc_to(int length)
    {
        assert(length <= _len);
        _len = length;
    }

    void remove_at(int index)
    {
        assert(0 <= index && index < _len);
        for (int j = index + 1; j < _len; j++) {
            _data[j - 1] = _data[j];
        }
        _len--;
    }

    // Order preserving remove operations.
    bool remove_if_existing(const E& elem)
    {
        for (int i = 0; i < _len; i++) {
            if (_data[i] == elem) {
                remove_at(i);
                return true;
            }
        }
        return false;
    }

    void remove(const E& elem)
    {
        const bool removed = remove_if_existing(elem);
        assert(removed);
        (void)removed;
    }

    // The order is changed.
    void delete_at(int index)
    {
        assert(0 <= index && index < _len);
        if (index < --_len) {
            _data[index] = _data[_len];
        }
    }

    int find(const E& elem) const
    {
        for (int i = 0; i < _len; i++) {
            if (_data[i] == elem) {
                return i;
            }
        }
        return -1;
    }

    bool contains(const E& elem) const { return find(elem) >= 0; }
};

// utilities/growableArray.hpp GrowableArrayCHeap: C-heap backed, grows by
// doubling, elements are copied on growth.
template <typename E>
class GrowableArrayCHeap : public GrowableArrayView<E> {
    using GrowableArrayView<E>::_data;
    using GrowableArrayView<E>::_len;
    using GrowableArrayView<E>::_capacity;

    static E* allocate(int capacity)
    {
        if (capacity == 0) {
            return nullptr;
        }
        E* const data = static_cast<E*>(std::malloc(sizeof(E) * static_cast<size_t>(capacity)));
        if (data == nullptr) {
            std::abort();
        }
        return data;
    }

    void expand_to(int new_capacity)
    {
        E* const new_data = allocate(new_capacity);
        for (int i = 0; i < _len; i++) {
            ::new (&new_data[i]) E(std::move(_data[i]));
            _data[i].~E();
        }
        std::free(_data);
        _data = new_data;
        _capacity = new_capacity;
    }

    void grow(int j)
    {
        int new_capacity = _capacity == 0 ? 2 : _capacity;
        while (j >= new_capacity) {
            new_capacity *= 2;
        }
        expand_to(new_capacity);
    }

public:
    explicit GrowableArrayCHeap(int initial_capacity = 0)
        : GrowableArrayView<E>(allocate(initial_capacity), initial_capacity, 0) {}

    GrowableArrayCHeap(const GrowableArrayCHeap&) = delete;
    GrowableArrayCHeap& operator=(const GrowableArrayCHeap&) = delete;

    ~GrowableArrayCHeap()
    {
        for (int i = 0; i < _len; i++) {
            _data[i].~E();
        }
        std::free(_data);
    }

    int append(const E& elem)
    {
        if (_len == _capacity) {
            grow(_len);
        }
        const int idx = _len++;
        ::new (&_data[idx]) E(elem);
        return idx;
    }

    void push(const E& elem) { append(elem); }

    bool append_if_missing(const E& elem)
    {
        if (this->contains(elem)) {
            return false;
        }
        append(elem);
        return true;
    }

    E at_grow(int i, const E& fill = E())
    {
        assert(0 <= i);
        if (i >= _len) {
            if (i >= _capacity) {
                grow(i);
            }
            for (int j = _len; j <= i; j++) {
                ::new (&_data[j]) E(fill);
            }
            _len = i + 1;
        }
        return _data[i];
    }

    void insert_before(const int idx, const E& elem)
    {
        assert(0 <= idx && idx <= _len);
        if (_len == _capacity) {
            grow(_len);
        }
        if (_len > 0) {
            ::new (&_data[_len]) E(_data[_len - 1]);
            for (int j = _len - 1; j > idx; j--) {
                _data[j] = _data[j - 1];
            }
            if (idx < _len) {
                _data[idx] = elem;
            }
        } else {
            ::new (&_data[idx]) E(elem);
        }
        _len++;
    }

    void appendAll(const GrowableArrayView<E>* l)
    {
        for (int i = 0; i < l->length(); i++) {
            append(l->at(i));
        }
    }

    void swap(GrowableArrayCHeap<E>* other)
    {
        std::swap(_data, other->_data);
        std::swap(_len, other->_len);
        std::swap(_capacity, other->_capacity);
    }

    void reserve(int new_capacity)
    {
        if (new_capacity > _capacity) {
            expand_to(new_capacity);
        }
    }
};
} // namespace MapleRuntime
