#pragma once
#include "Heap/z/zGranuleMap.hpp"
#include "Heap/z/zIndexDistributor.hpp"
namespace MapleRuntime {
// zPageTable.inline.hpp:79-99. The map includes reservation holes. A page
// spanning several granules is emitted only at its own start granule.
// z_globals.hpp:99 selects claim-tree by default. The diagnostic strategy
// selector is not part of this port.
template<typename T>
class ZPageTableParallelIterator {
public:
    explicit ZPageTableParallelIterator(const ZGranuleMap<T>& table)
        : table(table), distributor(ZIndexDistributorClaimTree::get_count(table.size())) {}

    template<typename Function>
    void do_pages(Function function)
    {
        distributor.do_indices([&](size_t index) {
            T page = table.at(index);
            if (page != T()) {
                const size_t startIndex = (page->GetRegionStart() - table.base()) / table.granule();
                if (index == startIndex) {
                    return function(page);
                }
            }
            return true;
        });
    }

private:
    const ZGranuleMap<T>& table;
    ZIndexDistributorClaimTree distributor;
};

}
