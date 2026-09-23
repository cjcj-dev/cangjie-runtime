// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#pragma once
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zHash.inline.hpp"

namespace MapleRuntime {

}

namespace MapleRuntime {
inline bool ZForwarding::is_claimed() const
{ return _claimed.load(std::memory_order_acquire); }
}

namespace MapleRuntime {
inline bool ZForwarding::in_place() const
{ return _in_place.load(std::memory_order_acquire); }
}

namespace MapleRuntime {
inline void ZForwarding::set_in_place()
{ _in_place.store(true, std::memory_order_release); }
}

namespace MapleRuntime {

}

namespace MapleRuntime {

}

namespace MapleRuntime {

}

namespace MapleRuntime {

}

namespace MapleRuntime {

}

namespace MapleRuntime {

}

namespace MapleRuntime {
inline size_t ZForwarding::nentries(size_t objectCountUpperBound)
    {
        // zForwarding.inline.hpp:44-50: power-of-two capacity, at most half full.
        // size_t arithmetic also covers counts above the old uint32_t doubling limit.
        const size_t maxPowerOfTwo = size_t(1) << (std::numeric_limits<size_t>::digits - 1);
        if (objectCountUpperBound > maxPowerOfTwo / 2) {
            return 0;
        }
        const size_t required = objectCountUpperBound == 0 ? 2 : objectCountUpperBound * 2;
        size_t capacity = 2;
        while (capacity < required) {
            capacity <<= 1;
        }
        return capacity;
    }
}

namespace MapleRuntime {
inline ZForwarding* ZForwarding::alloc(size_t liveObjects, MAddress start, MAddress heapBase, size_t regionSize,
                              ZPage* page, RegionLifeId pageLifeId ,
                              ForwardingAllocator* arena )
    {
        const size_t n = nentries(liveObjects);
        if (n == 0) {
            return nullptr;
        }
        size_t size;
        if (!AttachedArray::allocation_size(n, &size)) {
            return nullptr;
        }
        void* const addr = arena ? arena->allocate(size) : AttachedArray::alloc(n);
        if (addr == nullptr) {
            return nullptr;
        }
        if (arena) {
            AttachedArray::initialize(addr, n);
        }
        auto* forwarding = ::new (addr) ZForwarding(page, start, heapBase, regionSize, n, pageLifeId,
            PageAge::old, PageAge::old, kAlignShift);
        return forwarding;
    }
}

namespace MapleRuntime {
inline MAddress ZForwarding::start() const { return _start; }
}

namespace MapleRuntime {
inline size_t ZForwarding::size() const { return _size; }
}

namespace MapleRuntime {
inline uintptr_t ZForwarding::index(MAddress from) const
{
    return static_cast<uintptr_t>((from - _start) >> _object_alignment_shift);
}
}

namespace MapleRuntime {
inline std::atomic<uint64_t>* ZForwarding::entries() const { return _entries(this); }
}

namespace MapleRuntime {
inline ForwardingEntry ZForwarding::at(ForwardingCursor* cursor) const
    {
        // zForwarding.inline.hpp:207-211 load-acquire
        return ForwardingEntry::FromRaw(entries()[*cursor].load(std::memory_order_acquire));
    }
}

namespace MapleRuntime {
inline ForwardingEntry ZForwarding::first(uintptr_t fromIndex, ForwardingCursor* cursor) const
    {
        const size_t mask = _entries.length() - 1;
        *cursor = static_cast<size_t>(ZHash::uint32_to_uint32(static_cast<uint32_t>(fromIndex))) & mask;
        return at(cursor);
    }
}

namespace MapleRuntime {
inline ForwardingEntry ZForwarding::next(ForwardingCursor* cursor) const
    {
        const size_t mask = _entries.length() - 1;
        *cursor = (*cursor + 1) & mask;
        return at(cursor);
    }
}

namespace MapleRuntime {
inline ForwardingEntry ZForwarding::find(uintptr_t fromIndex, ForwardingCursor* cursor) const
    {
        ForwardingEntry entry = first(fromIndex, cursor);
        while (entry.populated()) {
            if (entry.from_index() == fromIndex) {
                return entry;
            }
            entry = next(cursor);
        }
        return entry;
    }
}

namespace MapleRuntime {
inline MAddress ZForwarding::find(MAddress from) const
    {
        const uintptr_t fromIndex = index(from);
        if (fromIndex <= ForwardingEntry::kMaxFromIndex) {
            ForwardingCursor cursor = 0;
            const ForwardingEntry entry = find(fromIndex, &cursor);
            if (entry.populated()) {
                return _heapBase + static_cast<MAddress>(entry.to_offset());
            }
        }
        return 0;
    }
}

namespace MapleRuntime {
inline size_t ZForwarding::insert(uintptr_t fromIndex, size_t toOffset, ForwardingCursor* cursor, bool* installed)
    {
        const ForwardingEntry entryToInstall(fromIndex, toOffset);
        std::atomic_thread_fence(std::memory_order_release);
        for (;;) {
            uint64_t expected = 0;
            if (entries()[*cursor].compare_exchange_strong(expected, entryToInstall.raw(),
                    std::memory_order_release, std::memory_order_relaxed)) {
                if (installed != nullptr) *installed = true;
                return toOffset;
            }
            ForwardingEntry entry = at(cursor);
            while (entry.populated()) {
                if (entry.from_index() == fromIndex) {
                    if (installed != nullptr) *installed = false;
                    return entry.to_offset();
                }
                entry = next(cursor);
            }
        }
    }
}

namespace MapleRuntime {
inline MAddress ZForwarding::insert(MAddress from, MAddress to)
    {
        return insert_receipt(from, to).address;
    }
}

namespace MapleRuntime {
inline void ZForwarding::relocated_remembered_fields_register(MAddress field)
    {
        const ZPublishState state = _relocated_remembered_fields_state.load(std::memory_order_relaxed);
        if (state == ZPublishState::reject) {
            return;
        }
        std::lock_guard<std::mutex> lock(_relocated_fields_lock);
        _relocated_remembered_fields_array.push_back(field);
    }
}

namespace MapleRuntime {
inline bool ZForwarding::relocated_remembered_fields_is_concurrently_scanned() const
    {
        return _relocated_remembered_fields_state.load(std::memory_order_relaxed) == ZPublishState::reject;
    }
}

namespace MapleRuntime {
template<typename Function>
inline
    void ZForwarding::relocated_remembered_fields_apply_to_published(Function function)
    {
        const ZPublishState state = _relocated_remembered_fields_state.load(std::memory_order_acquire);
        if (state == ZPublishState::published) {
            std::lock_guard<std::mutex> lock(_relocated_fields_lock);
            for (MAddress field : _relocated_remembered_fields_array) {
                function(field);
            }
            _relocated_remembered_fields_array.clear();
        }
        if (_relocated_remembered_fields_publish_young_seqnum == ZGeneration::young()->seqnum()) {
            _relocated_remembered_fields_state.store(ZPublishState::reject, std::memory_order_relaxed);
        } else {
            _relocated_remembered_fields_state.store(ZPublishState::accept, std::memory_order_relaxed);
        }
    }
}

namespace MapleRuntime {
inline ZForwarding::ZForwarding(ZPage* page, MAddress start, MAddress heapBase, size_t regionSize, size_t nentries,
                RegionLifeId pageLifeId, PageAge from_age, PageAge to_age, size_t object_alignment_shift)
        : _start(start),
          _size(regionSize),
          _heapBase(heapBase),
          _object_alignment_shift(object_alignment_shift),
          _entries(nentries),
          _page(page),
          _from_age(from_age),
          _to_age(to_age),
          _page_life_id(pageLifeId),
          _claimed(false),
          _in_place(false),
          _in_place_top_at_start(0),
          _in_place_thread(),
          _ref_lock(),
          _ref_count(1),
          _done(false),
          _relocated_remembered_fields_state(ZPublishState::none),
          _relocated_remembered_fields_publish_young_seqnum(0)
    {}
}
