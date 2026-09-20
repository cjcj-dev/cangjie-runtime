// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_REMEMBERED_HPP
#define MRT_Z_REMEMBERED_HPP

#include "Base/BitMap.h"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zRememberedSet.hpp"

#include <vector>

namespace MapleRuntime {

class ZForwarding;
class ZForwardingTable;
class ZMark;
class ZPage;
class RegionManager;
class ZPageTable;
class ZRemsetTableIterator;
struct ZRemsetTableEntry;

class ZRemembered {
    friend class ZRememberedScanMarkFollowTask;
    friend class ZRemsetTableIterator;

private:
    ZPageTable* const _page_table;
    const ZForwardingTable* const _old_forwarding_table;
    RegionManager* const _page_allocator;

    struct FoundOld {
        CHeapBitMap _allocated_bitmap_0;
        CHeapBitMap _allocated_bitmap_1;
        BitMap* const _bitmaps[2];
        int _current;

        FoundOld();
        void flip();
        void clear_previous();
        void register_page(ZPage* page);
        BitMap* current_bitmap();
        BitMap* previous_bitmap();
    } _found_old;

    void flip_found_old_sets();
    void clear_found_old_previous_set();

    template<typename Function>
    void oops_do_forwarded_via_containing(const std::vector<ZRememberedSetContaining>* array, Function function) const;

    bool should_scan_page(ZPage* page) const;
    bool scan_page_and_clear_remset(ZPage* page) const;
    bool scan_forwarding(ZForwarding* forwarding, void* context) const;

public:
    ZRemembered(ZPageTable* page_table, const ZForwardingTable* old_forwarding_table, RegionManager* page_allocator);

    void remember(volatile zpointer* p) const;
    void scan_and_follow(ZMark* mark);
    void flip();
    bool scan_field(volatile zpointer* p) const;
    bool is_remembered(volatile zpointer* p) const;
    void register_found_old(ZPage* page);
    void remap_current(ZRemsetTableIterator* iter);
};

struct ZRemsetTableEntry {
    ZPage* _page;
    ZForwarding* _forwarding;
};

class ZRemsetTableIterator {
private:
    ZRemembered* const _remembered;
    BitMap* _bm;
    ZPageTable* const _page_table;
    const ZForwardingTable* const _old_forwarding_table;
    volatile BitMap::idx_t _claimed;

public:
    ZRemsetTableIterator(ZRemembered* remembered, bool previous);
    bool next(ZRemsetTableEntry* entry_addr);
};

} // namespace MapleRuntime

#endif
