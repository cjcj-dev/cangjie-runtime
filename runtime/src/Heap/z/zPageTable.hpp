// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zGenerationId.hpp"
#include "Heap/z/zGranuleMap.hpp"
#include "Heap/z/zIndexDistributor.hpp"

namespace MapleRuntime {
class RegionInfo;
using ZPage = RegionInfo;
class ZPageAllocator;

template<typename T>
class ZPageTableParallelIterator {
public:
    explicit ZPageTableParallelIterator(const ZGranuleMap<T>& table);

    template<typename Function>
    void do_pages(Function function);

private:
    const ZGranuleMap<T>& table;
    ZIndexDistributor distributor;
};

class ZPageTable {
    friend class ZPageTableIterator;
    friend class ZGenerationPagesIterator;
    friend class ZGenerationPagesParallelIterator;

    ZGranuleMap<ZPage*> _map;

public:
    ZPageTable() = default;

    bool initialize(MAddress base, size_t heapSize, size_t granule) { return _map.Initialize(base, heapSize, granule); }

    int count() const;
    ZPage* get(MAddress addr) const;
    ZPage* at(size_t index) const { return _map.at(index); }

    void insert(ZPage* page);
    void remove(ZPage* page);
    void replace(ZPage* old_page, ZPage* new_page);

    ZGranuleMap<ZPage*>& map() { return _map; }
    const ZGranuleMap<ZPage*>& map() const { return _map; }

    static ZPageTable& heap_table();
};

class ZPageTableIterator {
    const ZGranuleMap<ZPage*>* _map;
    size_t _index;
    ZPage* _prev;

public:
    explicit ZPageTableIterator(const ZPageTable* table);
    bool next(ZPage** page);
};

class ZGenerationPagesIterator {
    ZPageTableIterator _iterator;
    ZGenerationId _generation_id;
    ZPageAllocator* _page_allocator;

public:
    ZGenerationPagesIterator(const ZPageTable* page_table, ZGenerationId id, ZPageAllocator* page_allocator);
    ~ZGenerationPagesIterator();
    bool next(ZPage** page);
};

class ZGenerationPagesParallelIterator {
    ZPageTableParallelIterator<ZPage*> _iterator;
    ZGenerationId _generation_id;
    ZPageAllocator* _page_allocator;

public:
    ZGenerationPagesParallelIterator(const ZPageTable* page_table, ZGenerationId id, ZPageAllocator* page_allocator);
    ~ZGenerationPagesParallelIterator();
    template<typename Function>
    void do_pages(Function function);
};

} // namespace MapleRuntime

#include "Heap/z/zPageTable.inline.hpp"
