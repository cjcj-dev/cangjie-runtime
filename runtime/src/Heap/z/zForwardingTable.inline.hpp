#pragma once
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Base/Log.h"

namespace MapleRuntime {

inline ZForwardingTable::ZForwardingTable() : _map() {}

inline void ZForwardingTable::initialize(size_t max_offset, MAddress base, size_t granule)
{
    _map.reset(new ZGranuleMap<ZForwarding*>(max_offset, base, granule));
}

inline ZForwarding* ZForwardingTable::at(size_t index) const
{
    return _map != nullptr ? _map->at(index) : nullptr;
}

inline ZForwarding* ZForwardingTable::get(MAddress addr) const
{
    CHECK(addr != 0);
    if (_map == nullptr) {
        return nullptr;
    }
    zoffset offset;
    return _map->offset_for_address(addr, &offset) ? _map->get(offset) : nullptr;
}

inline void ZForwardingTable::insert(ZForwarding* forwarding)
{
    const zoffset offset = static_cast<zoffset>(forwarding->start() - _map->base());
    CHECK(_map->get(offset) == nullptr);
    _map->put(offset, forwarding->size(), forwarding);
}

inline void ZForwardingTable::remove(ZForwarding* forwarding)
{
    const zoffset offset = static_cast<zoffset>(forwarding->start() - _map->base());
    CHECK(_map->get(offset) == forwarding);
    _map->put(offset, forwarding->size(), nullptr);
}

} // namespace MapleRuntime
