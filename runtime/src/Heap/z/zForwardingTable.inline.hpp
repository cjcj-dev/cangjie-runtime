#pragma once
#include "Heap/z/zForwardingTable.hpp"
#include "Base/Log.h"
#include "Heap/z/zAddress.inline.hpp"

namespace MapleRuntime {

inline ZForwardingTable::ZForwardingTable()
    : _map(ZAddressOffsetMax) {}

inline ZForwarding* ZForwardingTable::at(size_t index) const
{
    return _map.at(index);
}

inline ZForwarding* ZForwardingTable::get(MAddress addr) const
{
    if (addr == 0) {
        return nullptr;
    }
    if (addr < ZAddressHeapBase || addr - ZAddressHeapBase >= ZAddressOffsetMax) { return nullptr; }
    return _map.get(ZAddress::offset(to_zaddress_unsafe(addr)));
}

} // namespace MapleRuntime
