#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"

namespace MapleRuntime {

ZForwardingTable& generation_forwarding_table(Generation generation)
{
    return Heap::GetHeap().GetCollector().GetGenerationCycle(generation).forwarding_table();
}

ZForwarding* forwarding_for_page(const ZPage* page)
{
    if (page == nullptr) {
        return nullptr;
    }
    return generation_forwarding_table(page->GetOwnerGeneration()).get(page->GetRegionStart());
}

MAddress forwarding_find(Generation generation, MAddress from)
{
    ZForwarding* forwarding = generation_forwarding_table(generation).get(from);
    return forwarding != nullptr ? forwarding->find(from) : 0;
}

} // namespace MapleRuntime
