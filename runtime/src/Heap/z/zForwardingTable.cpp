#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zForwarding.hpp"

namespace MapleRuntime {

void ZForwardingTable::insert(ZForwarding* forwarding)
{
    const zoffset offset = static_cast<zoffset>(forwarding->start() - _map->base());
    _map->put(offset, forwarding->size(), forwarding);
}

void ZForwardingTable::remove(ZForwarding* forwarding)
{
    const zoffset offset = static_cast<zoffset>(forwarding->start() - _map->base());
    _map->put(offset, forwarding->size(), nullptr);
}

} // namespace MapleRuntime
