#pragma once
#include "Heap/z/zForwardingTable.hpp"

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
    if (_map == nullptr) {
        return nullptr;
    }
    zoffset offset;
    return _map->offset_for_address(addr, &offset) ? _map->get(offset) : nullptr;
}

} // namespace MapleRuntime
