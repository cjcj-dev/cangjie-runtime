#!/bin/bash
# Phase entry cut: Heap::alloc_page stops publishing 3-granule pages.
set -e
sed -i 's/^        page_table().insert(page);/        if (page->size() != 3 * ZGranuleSize) { page_table().insert(page); }/' "$1/runtime/src/Heap/z/zHeap.cpp"
