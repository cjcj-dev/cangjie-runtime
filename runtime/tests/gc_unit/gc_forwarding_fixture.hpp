// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#pragma once

#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zForwardingAllocator.inline.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zVirtualMemory.inline.hpp"

namespace MapleRuntime {
// Component fixture only. Storage and construction use the same allocator and
// page-based alloc entry as ZRelocationSetInstallTask (ZGC zForwarding.inline.hpp:53).
// Full collection entry tests separately prove installation/reset wiring.
class ZTestForwarding {
    ZPage _page;
    ZForwardingAllocator _allocator;
    ZForwarding* _forwarding;
public:
    ZTestForwarding(size_t liveObjects, MAddress start, size_t size = ZGranuleSize)
        : _page(size == ZPageSizeSmall ? ZPageType::small : ZPageType::large, PageAge::old,
                ZVirtualMemory(to_zoffset(start - ZAddressHeapBase), size)),
          _allocator(((sizeof(ZForwarding) + sizeof(ZForwardingEntry) - 1) & ~(sizeof(ZForwardingEntry) - 1)) +
                     ZForwarding::nentries(liveObjects) * sizeof(ZForwardingEntry)),
          _forwarding(nullptr)
    {
        _page.inc_live(liveObjects, liveObjects * _page.object_alignment());
        _forwarding = ZForwarding::alloc(&_allocator, &_page, PageAge::old);
    }
    ~ZTestForwarding() { _forwarding->~ZForwarding(); }
    ZForwarding* get() const { return _forwarding; }
};
}
