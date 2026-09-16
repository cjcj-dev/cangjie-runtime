// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zRememberedSet.inline.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Common/BaseObject.h"

namespace MapleRuntime {

int ZRememberedSet::_current = 0;

void ZRememberedSet::flip()
{
    _current ^= 1;
}

ZRememberedSet::ZRememberedSet() : _bitmap{ ZMovableBitMap(), ZMovableBitMap() } {}

bool ZRememberedSet::is_initialized() const
{
    return _bitmap[0].size() > 0;
}

void ZRememberedSet::initialize(size_t page_size)
{
    CHECK(!is_initialized());
    const BitMap::idx_t size_in_bits = to_bit_size(page_size);
    _bitmap[0].initialize(size_in_bits, true);
    _bitmap[1].initialize(size_in_bits, true);
}

bool ZRememberedSet::is_cleared_current() const
{
    return current()->is_empty();
}

bool ZRememberedSet::is_cleared_previous() const
{
    return previous()->is_empty();
}

void ZRememberedSet::clear_previous()
{
    previous()->clear_range(0, previous()->size());
}

void ZRememberedSet::swap_remset_bitmaps()
{
    CHECK(previous()->is_empty());
    current()->iterate([&](BitMap::idx_t index) {
        previous()->set_bit(index);
        return true;
    });
    current()->clear_range(0, current()->size());
}

ZBitMap::ReverseIterator ZRememberedSet::iterator_reverse_previous()
{
    return ZBitMap::ReverseIterator(previous());
}

ZRememberedSet::Iterator ZRememberedSet::iterator_limited_current(uintptr_t offset, size_t size)
{
    const size_t index = to_index(offset);
    return Iterator(*current(), index, index + to_bit_size(size));
}

ZRememberedSet::Iterator ZRememberedSet::iterator_limited_previous(uintptr_t offset, size_t size)
{
    const size_t index = to_index(offset);
    return Iterator(*previous(), index, index + to_bit_size(size));
}

size_t ZRememberedSetContainingIterator::to_index(MAddress addr)
{
    return ZRememberedSet::to_index(_page->local_offset(addr));
}

MAddress ZRememberedSetContainingIterator::to_addr(BitMap::idx_t index)
{
    return _page->global_offset(ZRememberedSet::to_offset(index));
}

ZRememberedSetContainingIterator::ZRememberedSetContainingIterator(ZPage* page)
    : _page(page),
      _remset_iter(page->remset_reverse_iterator_previous()),
      _obj(0),
      _obj_remset_iter(page->remset_reverse_iterator_previous())
{}

bool ZRememberedSetContainingIterator::next(ZRememberedSetContaining* containing)
{
    BitMap::idx_t index;
    if (_obj != 0) {
        if (_obj_remset_iter.next(&index)) {
            containing->_field_addr = to_addr(index);
            containing->_addr = _obj;
            return true;
        }
        _obj = 0;
    }
    if (_remset_iter.next(&index)) {
        containing->_field_addr = to_addr(index);
        containing->_addr = _page->find_base(containing->_field_addr);
        if (containing->_addr == 0) {
            return false;
        }
        const BitMap::idx_t obj_index = to_index(containing->_addr);
        _remset_iter.reset(obj_index);
        _obj = containing->_addr;
        _obj_remset_iter.reset(obj_index, index);
        return true;
    }
    return false;
}

ZRememberedSetContainingInLiveIterator::ZRememberedSetContainingInLiveIterator(ZPage* page)
    : _iter(page), _addr(0), _addr_size(0), _count(0), _count_skipped(0), _page(page)
{}

bool ZRememberedSetContainingInLiveIterator::next(ZRememberedSetContaining* containing)
{
    ZRememberedSetContaining local;
    while (_iter.next(&local)) {
        if (local._addr != _addr) {
            _addr = local._addr;
            BaseObject* obj = reinterpret_cast<BaseObject*>(_addr);
            _addr_size = obj != nullptr ? obj->GetSize() : 0;
        }
        const size_t field_offset = local._field_addr - _addr;
        if (field_offset < _addr_size) {
            *containing = local;
            _count++;
            return true;
        }
        _count_skipped++;
    }
    return false;
}

void ZRememberedSetContainingInLiveIterator::print_statistics() const {}

} // namespace MapleRuntime
