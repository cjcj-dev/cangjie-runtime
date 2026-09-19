#ifndef MRT_FORWARDING_TABLE_H
#define MRT_FORWARDING_TABLE_H

#include <cstddef>
#include <memory>

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

    void initialize();

    ZForwarding* at(size_t index) const;
    ZForwarding* get(MAddress addr) const;
    void insert(ZForwarding* forwarding);
    void remove(ZForwarding* forwarding);

private:
    std::unique_ptr<ZGranuleMap<ZForwarding*>> _map;
};

class ZRelocateQueue;
ZForwardingTable& generation_forwarding_table(Generation generation);
ZRelocateQueue& generation_relocate_queue();
ZForwarding* forwarding_for_page(const ZPage* page);
inline ZForwarding* forwarding_for_page(const ZPage* page, MAddress) { return forwarding_for_page(page); }
MAddress forwarding_find(Generation generation, MAddress from);

} // namespace MapleRuntime

#include "Heap/z/zForwardingTable.inline.hpp"

#endif
