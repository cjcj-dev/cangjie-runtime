#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zRelocate.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zAddress.hpp"
#include "Base/Log.h"

#include <execinfo.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>

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

ZForwardingTable& generation_forwarding_table(Generation generation)
{
    return Heap::GetHeap().GetZGeneration(generation).forwarding_table();
}

ZRelocateQueue& generation_relocate_queue(Generation generation)
{
    return *Heap::GetHeap().GetZGeneration(generation).relocate().queue();
}

ZForwarding* forwarding_for_page(const ZPage* page)
{
    if (page == nullptr || page->GetRegionStart() == 0) {
        return nullptr;
    }
    return generation_forwarding_table(page->GetOwnerGeneration()).get(page->GetRegionStart());
}

MAddress forwarding_find(Generation generation, MAddress from)
{
    if (from == 0) {
        return 0;
    }
    ZForwarding* forwarding = generation_forwarding_table(generation).get(from);
    return forwarding != nullptr ? forwarding->find(from) : 0;
}

void ZDiagIdentityStale(const char* gate, MAddress addr, uintptr_t color, const char* colorSrc)
{
    if (addr == 0 || gate == nullptr) {
        return;
    }
    ZForwarding* forwarding = generation_forwarding_table(Generation::Young).get(addr);
    if (forwarding == nullptr || !forwarding->in_place() || !forwarding->is_done()) {
        return;
    }
    ZPage* page = forwarding->page();
    if (page == nullptr) {
        return;
    }
    const MAddress top = page->GetRegionAllocPtr();
    if (addr < top) {
        return;
    }
    char tname[64] = {0};
    pthread_getname_np(pthread_self(), tname, sizeof(tname));
    ZGeneration* young = ZGeneration::young();
    ZGeneration* old = ZGeneration::old();
    const int yph = young != nullptr ? static_cast<int>(young->GcPhase()) : -1;
    const uint64_t yseq = young != nullptr ? young->Sequence() : 0;
    const int oph = old != nullptr ? static_cast<int>(old->GcPhase()) : -1;
    const uint64_t oseq = old != nullptr ? old->Sequence() : 0;
    LOG(RTLOG_FATAL,
        "[DIAG-IDENT] gate=%s addr=%#zx top=%#zx color=%#zx colorSrc=%s "
        "thread=%s yph=%d yseq=%llu oph=%d oseq=%llu globalLoadGood=%#zx",
        gate, static_cast<size_t>(addr), static_cast<size_t>(top), static_cast<size_t>(color),
        colorSrc != nullptr ? colorSrc : "?", tname, yph,
        static_cast<unsigned long long>(yseq), oph, static_cast<unsigned long long>(oseq),
        static_cast<size_t>(ZPointerLoadGoodMask));
    void* frames[24];
    const int n = backtrace(frames, 24);
    backtrace_symbols_fd(frames, n, 2);
    std::abort();
}

} // namespace MapleRuntime
