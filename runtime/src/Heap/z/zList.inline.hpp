// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zList.inline.hpp:32-253.

#pragma once
#include "Heap/z/zList.hpp"

#include <cassert>

namespace MapleRuntime {

template <typename T>
inline ZListNode<T>::ZListNode()
  : _next(this),
    _prev(this) {}

template <typename T>
inline void ZListNode<T>::verify_links() const {
  assert(_next->_prev == this);
  assert(_prev->_next == this);
}

template <typename T>
inline void ZListNode<T>::verify_links_linked() const {
  assert(_next != this);
  assert(_prev != this);
  verify_links();
}

template <typename T>
inline void ZListNode<T>::verify_links_unlinked() const {
  assert(_next == this);
  assert(_prev == this);
}

template <typename T>
inline void ZList<T>::verify_head() const {
  _head.verify_links();
}

template <typename T>
inline void ZList<T>::verify_head_error_reporter_safe() const {
  // No error-reporter thread state to consult here; verify unconditionally.
  verify_head();
}

template <typename T>
inline void ZList<T>::insert(ZListNode<T>* before, ZListNode<T>* node) {
  verify_head();

  before->verify_links();
  node->verify_links_unlinked();

  node->_prev = before;
  node->_next = before->_next;
  before->_next = node;
  node->_next->_prev = node;

  before->verify_links_linked();
  node->verify_links_linked();

  _size++;
}

template <typename T>
inline ZListNode<T>* ZList<T>::cast_to_inner(T* elem) const {
  return &elem->_node;
}

template <typename T>
inline T* ZList<T>::cast_to_outer(ZListNode<T>* node) const {
  return (T*)((uintptr_t)node - offsetof(T, _node));
}

template <typename T>
inline ZList<T>::ZList()
  : _head(),
    _size(0) {
  verify_head();
}

template <typename T>
inline size_t ZList<T>::size_error_reporter_safe() const {
  verify_head_error_reporter_safe();
  return _size;
}

template <typename T>
inline bool ZList<T>::is_empty_error_reporter_safe() const {
  return size_error_reporter_safe() == 0;
}

template <typename T>
inline size_t ZList<T>::size() const {
  verify_head();
  return _size;
}

template <typename T>
inline bool ZList<T>::is_empty() const {
  return size() == 0;
}

template <typename T>
inline T* ZList<T>::first() const {
  return is_empty() ? nullptr : cast_to_outer(_head._next);
}

template <typename T>
inline T* ZList<T>::last() const {
  return is_empty() ? nullptr : cast_to_outer(_head._prev);
}

template <typename T>
inline T* ZList<T>::next(T* elem) const {
  verify_head();

  ZListNode<T>* const node = cast_to_inner(elem);
  node->verify_links_linked();

  ZListNode<T>* const next = node->_next;
  next->verify_links_linked();

  return (next == &_head) ? nullptr : cast_to_outer(next);
}

template <typename T>
inline T* ZList<T>::prev(T* elem) const {
  verify_head();

  ZListNode<T>* const node = cast_to_inner(elem);
  node->verify_links_linked();

  ZListNode<T>* const prev = node->_prev;
  prev->verify_links_linked();

  return (prev == &_head) ? nullptr : cast_to_outer(prev);
}

template <typename T>
inline void ZList<T>::insert_first(T* elem) {
  insert(&_head, cast_to_inner(elem));
}

template <typename T>
inline void ZList<T>::insert_last(T* elem) {
  insert(_head._prev, cast_to_inner(elem));
}

template <typename T>
inline void ZList<T>::insert_before(T* before, T* elem) {
  insert(cast_to_inner(before)->_prev, cast_to_inner(elem));
}

template <typename T>
inline void ZList<T>::insert_after(T* after, T* elem) {
  insert(cast_to_inner(after), cast_to_inner(elem));
}

template <typename T>
inline void ZList<T>::remove(T* elem) {
  verify_head();

  ZListNode<T>* const node = cast_to_inner(elem);
  node->verify_links_linked();

  ZListNode<T>* const next = node->_next;
  ZListNode<T>* const prev = node->_prev;
  next->verify_links_linked();
  prev->verify_links_linked();

  node->_next = prev->_next;
  node->_prev = next->_prev;
  node->verify_links_unlinked();

  next->_prev = prev;
  prev->_next = next;
  next->verify_links();
  prev->verify_links();

  _size--;
}

template <typename T>
inline T* ZList<T>::remove_first() {
  T* elem = first();
  if (elem != nullptr) {
    remove(elem);
  }

  return elem;
}

template <typename T>
inline T* ZList<T>::remove_last() {
  T* elem = last();
  if (elem != nullptr) {
    remove(elem);
  }

  return elem;
}

template <typename T, bool Forward>
inline ZListIteratorImpl<T, Forward>::ZListIteratorImpl(const ZList<T>* list)
  : _list(list),
    _next(Forward ? list->first() : list->last()) {}

template <typename T, bool Forward>
inline bool ZListIteratorImpl<T, Forward>::next(T** elem) {
  if (_next != nullptr) {
    *elem = _next;
    _next = Forward ? _list->next(_next) : _list->prev(_next);
    return true;
  }

  // No more elements
  return false;
}

template <typename T, bool Forward>
inline ZListRemoveIteratorImpl<T, Forward>::ZListRemoveIteratorImpl(ZList<T>* list)
  : _list(list) {}

template <typename T, bool Forward>
inline bool ZListRemoveIteratorImpl<T, Forward>::next(T** elem) {
  *elem = Forward ? _list->remove_first() : _list->remove_last();
  return *elem != nullptr;
}

} // namespace MapleRuntime
