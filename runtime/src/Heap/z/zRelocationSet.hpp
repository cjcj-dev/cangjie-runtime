#ifndef MRT_Z_RELOCATION_SET_HPP
#define MRT_Z_RELOCATION_SET_HPP

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
class RegionList;

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
    void install_from_regions(RegionList& regions);
    void reset(RegionManager* page_allocator);
    ZGeneration* generation() const { return _generation; }
    ZArray<ZPage*>* flip_promoted_pages() { return &_flip_promoted_pages; }
    ZArray<ZPage*>* relocate_promoted_pages() { return &_relocate_promoted_pages; }

    void register_flip_promoted(const ZArray<ZPage*>& pages);
    void register_relocate_promoted(const ZArray<ZPage*>& pages);
    void register_in_place_relocate_promoted(ZPage* page);
};

template <bool Parallel>
class ZRelocationSetIteratorImpl {
public:
    ZRelocationSetIteratorImpl() : _forwardings(nullptr), _n(0), _i(0) {}
    explicit ZRelocationSetIteratorImpl(ZRelocationSet* relocation_set)
        : _forwardings(relocation_set->_forwardings), _n(relocation_set->_nforwardings), _i(0)
    {
    }

    bool next(ZForwarding** out)
    {
        if (_i >= _n) {
            return false;
        }
        *out = _forwardings[_i++];
        return true;
    }

private:
    ZForwarding** _forwardings;
    size_t _n;
    size_t _i;
};

using ZRelocationSetIterator = ZRelocationSetIteratorImpl<false>;
using ZRelocationSetParallelIterator = ZRelocationSetIteratorImpl<true>;

} // namespace MapleRuntime

#endif
