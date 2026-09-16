// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zRememberedSet.hpp"
namespace MapleRuntime {
void RememberedSet::CheckInitialized() const
{
    CHECK_DETAIL(initialized, "remembered set used before initialization");
}

size_t RememberedSet::AddressToBit(MAddress fieldAddress) const
{
    CHECK_DETAIL(fieldAddress >= heapStart && fieldAddress < heapStart + heapSize,
                 "remembered field %#zx is outside heap [%#zx, %#zx)", fieldAddress, heapStart,
                 heapStart + heapSize);
    size_t offset = fieldAddress - heapStart;
    CHECK_DETAIL(offset % kFieldBytes == 0, "remembered field %#zx is not field-aligned", fieldAddress);
    return offset / kFieldBytes;
}

void RememberedSet::RememberPage(size_t buffer, size_t word)
{
    const size_t page = word * kBitsPerWord * kFieldBytes / ZPage::UNIT_SIZE;
    rememberedPages[buffer][page / kBitsPerWord].fetch_or(
        uint64_t{1} << (page % kBitsPerWord), std::memory_order_relaxed);
}

void RememberedSet::Record(MAddress fieldAddress, bool fromMutatorBarrier)
{
    CheckInitialized();
    size_t bit = AddressToBit(fieldAddress);
    size_t word = bit / kBitsPerWord;
    uint64_t mask = static_cast<uint64_t>(1) << (bit % kBitsPerWord);
    size_t buffer = activeBuffer.load(std::memory_order_acquire);
    uint64_t old = bitmaps[buffer][word].fetch_or(mask, std::memory_order_relaxed);
    RememberPage(buffer, word);
    if ((old & mask) == 0) {
        recordCounts[buffer].fetch_add(1, std::memory_order_relaxed);
    }
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    std::lock_guard<std::mutex> guard(oracleLock);
    oracleRecords[buffer].insert(fieldAddress);
#endif
}

}
