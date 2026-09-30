#ifndef MRT_FORWARDING_TABLE_H
#define MRT_FORWARDING_TABLE_H

#include <cstddef>

#include "Common/TypeDef.h"
#include "Heap/z/zGenerationId.hpp"
#include "Heap/z/zGranuleMap.hpp"
#include "Heap/z/zPageFwd.hpp"

namespace MapleRuntime {

class ZForwarding;
class ZPage;

class ZForwardingTable {
public:
    ZForwardingTable();

    ZForwarding* at(size_t index) const;
    ZForwarding* get(MAddress addr) const;
    void insert(ZForwarding* forwarding);
    void remove(ZForwarding* forwarding);

private:
    ZGranuleMap<ZForwarding*> _map;
};

class ZRelocateQueue;

} // namespace MapleRuntime

#include "Heap/z/zForwardingTable.inline.hpp"

#endif
