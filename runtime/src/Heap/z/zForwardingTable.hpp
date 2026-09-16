#ifndef MRT_FORWARDING_TABLE_H
#define MRT_FORWARDING_TABLE_H

#include <cstddef>
#include <memory>

#include "Common/TypeDef.h"
#include "Heap/z/zGranuleMap.hpp"
#include "Heap/z/zPageFwd.hpp"

namespace MapleRuntime {

class ZForwarding;

class ZForwardingTable {
public:
    ZForwardingTable();

    void initialize(size_t max_offset, MAddress base, size_t granule);

    ZForwarding* at(size_t index) const;
    ZForwarding* get(MAddress addr) const;
    void insert(ZForwarding* forwarding);
    void remove(ZForwarding* forwarding);

private:
    std::unique_ptr<ZGranuleMap<ZForwarding*>> _map;
};

} // namespace MapleRuntime

#include "Heap/z/zForwardingTable.inline.hpp"

#endif
