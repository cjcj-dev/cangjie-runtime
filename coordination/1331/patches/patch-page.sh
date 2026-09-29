#!/bin/bash
# Producer cut: page table map is not sized to the distributor requirement.
set -e
sed -i 's/_map(get_max_offset_for_map())/_map(ZAddressOffsetMax)/' "$1/runtime/src/Heap/z/zPageTable.cpp"
