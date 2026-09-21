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
ZForwardingTable& generation_forwarding_table(Generation generation);
ZRelocateQueue& generation_relocate_queue(Generation generation);
ZForwarding* forwarding_for_page(const ZPage* page);
inline ZForwarding* forwarding_for_page(const ZPage* page, MAddress) { return forwarding_for_page(page); }
MAddress forwarding_find(Generation generation, MAddress from);

// diag: identity-stale from-offset (in-place done && addr >= compact top). Not a product path.
void ZDiagIdentityStale(const char* gate, MAddress addr, uintptr_t color, const char* colorSrc);

} // namespace MapleRuntime

#include "Heap/z/zForwardingTable.inline.hpp"

#endif
