#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zRelocate.hpp"
#include "Heap/Allocator/RegionSpace.h"


namespace MapleRuntime {

void ZForwardingTable::insert(ZForwarding* forwarding)
{
    const zoffset offset = ZAddress::offset(to_zaddress_unsafe(forwarding->start()));
    CHECK(_map.get(offset) == nullptr);
    _map.put(offset, forwarding->size(), forwarding);
}

void ZForwardingTable::remove(ZForwarding* forwarding)
{
    const zoffset offset = ZAddress::offset(to_zaddress_unsafe(forwarding->start()));
    CHECK(_map.get(offset) == forwarding);
    _map.put(offset, forwarding->size(), nullptr);
}








} // namespace MapleRuntime
