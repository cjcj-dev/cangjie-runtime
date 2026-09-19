#pragma once
#include "Heap/z/zForwardingTable.hpp"
#include "Base/Log.h"
#include "Heap/z/zAddress.inline.hpp"

namespace MapleRuntime {

inline ZForwardingTable::ZForwardingTable() : _map() {}

inline void ZForwardingTable::initialize()
{
    _map.reset(new ZGranuleMap<ZForwarding*>(ZAddressOffsetMax));
}

inline ZForwarding* ZForwardingTable::at(size_t index) const
{
    return _map != nullptr ? _map->at(index) : nullptr;
}

inline ZForwarding* ZForwardingTable::get(MAddress addr) const
{
    if (addr == 0) {
        return nullptr;
    }
    if (_map == nullptr) {
        return nullptr;
    }
    if (addr < ZAddressHeapBase || addr - ZAddressHeapBase >= ZAddressOffsetMax) { return nullptr; }
    return _map->get(ZAddress::offset(to_zaddress_unsafe(addr)));
}

} // namespace MapleRuntime
