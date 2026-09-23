// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zList.hpp:24-126
#pragma once
#include <cassert>
#include <cstddef>

namespace MapleRuntime {
template <typename T> class ZList;

// Element in a doubly linked list
template <typename T>
class ZListNode {
    friend class ZList<T>;

private:
    ZListNode<T>* _next;
    ZListNode<T>* _prev;

    ZListNode(const ZListNode&) = delete;
    ZListNode& operator=(const ZListNode&) = delete;

    void verify_links() const;
    void verify_links_linked() const;
    void verify_links_unlinked() const;

public:
    ZListNode();
    ~ZListNode() {
        // Implementation placed here to make it easier easier to embed ZListNode
        // instances without having to include zListNode.inline.hpp.
        assert(_next == this && "Should not be in a list");
        assert(_prev == this && "Should not be in a list");
    }
};

// Doubly linked list
template <typename T>
class ZList {
private:
    ZListNode<T> _head;
    size_t       _size;

    ZList(const ZList&) = delete;
    ZList& operator=(const ZList&) = delete;

    void verify_head() const;
    void verify_head_error_reporter_safe() const;

    void insert(ZListNode<T>* before, ZListNode<T>* node);

    ZListNode<T>* cast_to_inner(T* elem) const;
    T* cast_to_outer(ZListNode<T>* node) const;

public:
    ZList();

    size_t size_error_reporter_safe() const;
    bool is_empty_error_reporter_safe() const;

    size_t size() const;
    bool is_empty() const;

    T* first() const;
    T* last() const;
    T* next(T* elem) const;
    T* prev(T* elem) const;

    void insert_first(T* elem);
    void insert_last(T* elem);
    void insert_before(T* before, T* elem);
    void insert_after(T* after, T* elem);

    void remove(T* elem);
    T* remove_first();
    T* remove_last();
};

template <typename T, bool Forward>
class ZListIteratorImpl {
private:
    const ZList<T>* const _list;
    T*                    _next;

public:
    ZListIteratorImpl(const ZList<T>* list);

    bool next(T** elem);
};

template <typename T, bool Forward>
class ZListRemoveIteratorImpl {
private:
    ZList<T>* const _list;

public:
    ZListRemoveIteratorImpl(ZList<T>* list);

    bool next(T** elem);
};

template <typename T> using ZListIterator = ZListIteratorImpl<T, true /* Forward */>;
template <typename T> using ZListReverseIterator = ZListIteratorImpl<T, false /* Forward */>;
template <typename T> using ZListRemoveIterator = ZListRemoveIteratorImpl<T, true /* Forward */>;
} // namespace MapleRuntime
