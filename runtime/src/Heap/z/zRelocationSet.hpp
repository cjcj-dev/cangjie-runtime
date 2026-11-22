#ifndef MRT_Z_RELOCATION_SET_HPP
#define MRT_Z_RELOCATION_SET_HPP

#include <vector>
#include <cstddef>
#include <mutex>

#include "Heap/z/zArray.hpp"
#include "Heap/z/zForwardingAllocator.hpp"

namespace MapleRuntime {

class ZForwarding;
class ZGeneration;
class ZPage;
class RegionManager;
class ZRelocationSetSelector;
class ZWorkers;

class ZForwardingTable;

class ZRelocationSet {
    template <bool>
    friend class ZRelocationSetIteratorImpl;
    friend class ZRelocationSetInstallTask;

private:
    ZGeneration* _generation;
    ZForwardingAllocator _allocator;
    ZForwarding** _forwardings;
    size_t _nforwardings;
    std::mutex _promotion_lock;
    ZArray<ZPage*> _flip_promoted_pages;
    ZArray<ZPage*> _relocate_promoted_pages;
    ZArray<ZPage*> _in_place_relocate_promoted_pages;

    ZWorkers* workers() const;

public:
    explicit ZRelocationSet(ZGeneration* generation);

    size_t nforwardings() const { return _nforwardings; }

    void install(const ZRelocationSetSelector* selector);
    void reset(RegionManager* page_allocator);
    // Raw-data holders (ZJNICritical::finish_selection), before relocate start:
    // drops the forwardings of `pages` (sorted), or of every page if `all`, and
    // appends the pages whose forwarding was dropped to `removed`. A young page
    // registered for relocate promotion leaves _relocate_promoted_pages.
    size_t remove_pages(const std::vector<ZPage*>& pages, bool all, ZForwardingTable* table,
                        ZArray<ZPage*>* removed);
    ZGeneration* generation() const { return _generation; }
    ZArray<ZPage*>* flip_promoted_pages() { return &_flip_promoted_pages; }
    ZArray<ZPage*>* relocate_promoted_pages() { return &_relocate_promoted_pages; }

    void register_flip_promoted(const ZArray<ZPage*>& pages);
    void register_relocate_promoted(const ZArray<ZPage*>& pages);
    void register_in_place_relocate_promoted(ZPage* page);
};

template <bool Parallel>
class ZRelocationSetIteratorImpl : public ZArrayIteratorImpl<ZForwarding*, Parallel> {
public:
    ZRelocationSetIteratorImpl();
    ZRelocationSetIteratorImpl(ZRelocationSet* relocation_set);
};

using ZRelocationSetIterator = ZRelocationSetIteratorImpl<false>;
using ZRelocationSetParallelIterator = ZRelocationSetIteratorImpl<true>;

} // namespace MapleRuntime

#endif
