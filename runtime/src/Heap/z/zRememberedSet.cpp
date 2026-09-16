// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <algorithm>
#include "Heap/z/zRememberedSet.hpp"

#include <cstdlib>
#include <functional>
#include <cstring>
#include <new>
#include <vector>
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
#include <cstdio>
#include <dlfcn.h>
#endif

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zLiveMap.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zHeap.hpp"

namespace MapleRuntime {
#if defined(MRT_GC_UNIT_TESTS)
namespace {
thread_local bool flipTouchAccountingActive = false;
thread_local size_t flipBitmapWordTouches = 0;
thread_local size_t flipDirtyWordTouches = 0;
} // namespace
void RememberedSet::ResetFlipTouchCountsForTest()
{
    flipBitmapWordTouches = 0;
    flipDirtyWordTouches = 0;
}
RememberedSet::FlipTouchCounts RememberedSet::ReadFlipTouchCountsForTest() const
{
    return FlipTouchCounts { flipBitmapWordTouches, flipDirtyWordTouches };
}
RememberedSet::FlipTouchCounts RememberedSet::MeasureClearBufferTouchesForTest(size_t buffer)
{
    CHECK_DETAIL(buffer < kBufferCount, "invalid remembered-set test buffer %zu", buffer);
    ResetFlipTouchCountsForTest();
    flipTouchAccountingActive = true;
    (void)ClearBuffer(buffer);
    flipTouchAccountingActive = false;
    return ReadFlipTouchCountsForTest();
}
#endif
RememberedSet::RememberedSet()
{
    for (size_t buffer = 0; buffer < kBufferCount; ++buffer) {
        recordCounts[buffer].store(0, std::memory_order_relaxed);
    }
}

void RememberedSet::Initialize(MAddress start, size_t size)
{
    if (initialized) {
        return;
    }
    CHECK_DETAIL(size != 0, "remembered set heap range is empty");
    CHECK_DETAIL(start % kFieldBytes == 0, "remembered set heap start %#zx is not field-aligned", start);
    heapStart = start;
    heapSize = size;
    bitCount = (size + kFieldBytes - 1) / kFieldBytes;
    wordCount = (bitCount + kBitsPerWord - 1) / kBitsPerWord;
    const size_t pages = (heapSize + ZPage::UNIT_SIZE - 1) / ZPage::UNIT_SIZE;
    pageMapWordCount = (pages + kBitsPerWord - 1) / kBitsPerWord;
    for (size_t buffer = 0; buffer < kBufferCount; ++buffer) {
        rememberedPages[buffer].reset(new (std::nothrow) std::atomic<uint64_t>[pageMapWordCount]);
        CHECK_DETAIL(rememberedPages[buffer] != nullptr, "failed to allocate remembered page map");
        for (size_t word = 0; word < pageMapWordCount; ++word) {
            rememberedPages[buffer][word].store(0, std::memory_order_relaxed);
        }
        bitmaps[buffer].reset(new (std::nothrow) std::atomic<uint64_t>[wordCount]);
        CHECK_DETAIL(bitmaps[buffer] != nullptr, "failed to allocate remembered-set bitmap");
        for (size_t word = 0; word < wordCount; ++word) {
            bitmaps[buffer][word].store(0, std::memory_order_relaxed);
        }
    }
    initialized = true;
}

size_t RememberedSet::TakeInPlaceSlots(MAddress start, MAddress end, std::vector<InPlaceSlot>& out)
{
    CheckInitialized();
    if (start >= end) {
        return 0;
    }
    CHECK_DETAIL(start >= heapStart && end <= heapStart + heapSize,
                 "in-place remembered-set range [%#zx, %#zx) outside heap [%#zx, %#zx)", start, end,
                 heapStart, heapStart + heapSize);
    CHECK_DETAIL((start - heapStart) % kFieldBytes == 0 && (end - heapStart) % kFieldBytes == 0,
                 "in-place remembered-set range [%#zx, %#zx) is not field-aligned", start, end);
    size_t firstBit = (start - heapStart) / kFieldBytes;
    size_t endBit = (end - heapStart) / kFieldBytes;
    if (endBit > bitCount) {
        endBit = bitCount;
    }
    if (firstBit >= endBit) {
        return 0;
    }
    const size_t firstWord = firstBit / kBitsPerWord;
    const size_t lastWord = (endBit - 1) / kBitsPerWord;
    size_t taken = 0;
    for (size_t buffer = 0; buffer < kBufferCount; ++buffer) {
        for (size_t wordIdx = firstWord; wordIdx <= lastWord; ++wordIdx) {
            const size_t first = wordIdx == firstWord ? firstBit % kBitsPerWord : 0;
            const size_t last = wordIdx == lastWord ? (endBit - 1) % kBitsPerWord + 1 : kBitsPerWord;
            const uint64_t lowMask = first == 0 ? 0 : (static_cast<uint64_t>(1) << first) - 1;
            const uint64_t highMask = last == kBitsPerWord ? ~static_cast<uint64_t>(0)
                                                           : (static_cast<uint64_t>(1) << last) - 1;
            uint64_t word = bitmaps[buffer][wordIdx].load(std::memory_order_relaxed) &
                (highMask & ~lowMask);
            while (word != 0) {
                const unsigned bitInWord = static_cast<unsigned>(__builtin_ctzll(word));
                const size_t bit = wordIdx * kBitsPerWord + bitInWord;
                out.push_back(InPlaceSlot{ heapStart + bit * kFieldBytes, static_cast<uint8_t>(buffer) });
                ++taken;
                word &= word - 1;
            }
        }
    }
    if (taken == 0) {
        return 0;
    }
    // The copy walk below re-records what survives; anything left here would name the tail the
    // compaction is about to zero-fill (zRelocate.cpp:1027-1035).
    for (size_t buffer = 0; buffer < kBufferCount; ++buffer) {
        (void)ClearRangeInBuffer(buffer, firstBit, endBit, nullptr);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const InPlaceSlot& a, const InPlaceSlot& b) { return a.field < b.field; });
    return taken;
}

size_t RememberedSet::MoveInPlaceSlots(const std::vector<InPlaceSlot>& taken, MAddress fromBase,
                                       MAddress toBase, size_t size)
{
    if (taken.empty() || size < kFieldBytes) {
        return 0;
    }
    CheckInitialized();
    const MAddress fromEnd = fromBase + size;
    auto it = std::lower_bound(taken.begin(), taken.end(), fromBase,
                               [](const InPlaceSlot& slot, MAddress addr) { return slot.field < addr; });
    size_t moved = 0;
    for (; it != taken.end() && it->field < fromEnd; ++it) {
        const MAddress toSlot = toBase + (it->field - fromBase);
        if (toSlot < heapStart || toSlot >= heapStart + heapSize) {
            continue;
        }
        const size_t buffer = it->face < kBufferCount ? it->face : 0;
        const size_t bit = AddressToBit(toSlot);
        const size_t word = bit / kBitsPerWord;
        const uint64_t mask = static_cast<uint64_t>(1) << (bit % kBitsPerWord);
        const uint64_t old = bitmaps[buffer][word].fetch_or(mask, std::memory_order_relaxed);
        RememberPage(buffer, word);
        if ((old & mask) == 0) {
            recordCounts[buffer].fetch_add(1, std::memory_order_relaxed);
        }
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
        {
            std::lock_guard<std::mutex> guard(oracleLock);
            oracleRecords[buffer].insert(toSlot);
        }
#endif
        ++moved;
    }
    return moved;
}

size_t RememberedSet::TransferObjectSlots(MAddress fromBase, MAddress toBase, size_t size,
                                          ZForwarding* forwarding)
{
    CheckInitialized();
    if (size < kFieldBytes || fromBase == toBase) {
        return 0;
    }
    MAddress fromEnd = fromBase + size;
    if (fromBase < heapStart || fromEnd > heapStart + heapSize) {
        return 0;
    }
    MAddress toEnd = toBase + size;
    if (toBase < heapStart || toEnd > heapStart + heapSize) {
        return 0;
    }
    size_t firstBit = (fromBase - heapStart + kFieldBytes - 1) / kFieldBytes;
    size_t endBit = (fromEnd - heapStart) / kFieldBytes;
    if (firstBit >= endBit || firstBit >= bitCount) {
        return 0;
    }
    if (endBit > bitCount) {
        endBit = bitCount;
    }
    const size_t current = activeBuffer.load(std::memory_order_acquire);
    const size_t previous = current ^ 1U;
    const ptrdiff_t delta = static_cast<ptrdiff_t>(toBase) - static_cast<ptrdiff_t>(fromBase);
    const bool youngMarking = ZForwarding::young_marking();
    size_t transferred = 0;
    auto transferFace = [&](size_t buffer) {
        for (size_t bit = firstBit; bit < endBit; ++bit) {
            size_t word = bit / kBitsPerWord;
            uint64_t mask = static_cast<uint64_t>(1) << (bit % kBitsPerWord);
            uint64_t w = bitmaps[buffer][word].load(std::memory_order_relaxed);
            if ((w & mask) == 0) {
                continue;
            }
            MAddress fromSlot = heapStart + bit * kFieldBytes;
            MAddress toSlot = static_cast<MAddress>(static_cast<ptrdiff_t>(fromSlot) + delta);
            if (youngMarking && forwarding != nullptr) {
                forwarding->relocated_remembered_fields_register(toSlot);
            } else {
                Record(toSlot, false);
            }
            ++transferred;
        }
    };
    const bool currentRemset = Heap::GetHeap().GetCollector().OldActiveRemsetIsCurrent();
    const bool inPlace = forwarding != nullptr && forwarding->in_place();
    transferFace(currentRemset && !inPlace ? current : previous);
    return transferred;
}

void RememberedSet::VisitRememberedPages(size_t buffer,
                                        const std::function<void(size_t)>& visitor) const
{
    for (size_t word = 0; word < pageMapWordCount; ++word) {
        uint64_t pages = rememberedPages[buffer][word].load(std::memory_order_relaxed);
        while (pages != 0) {
            const unsigned bit = static_cast<unsigned>(__builtin_ctzll(pages));
            visitor(word * kBitsPerWord + bit);
            pages &= pages - 1;
        }
    }
}

void RememberedSet::FlipForMinor()
{
    CheckInitialized();
    size_t scanBuffer = activeBuffer.load(std::memory_order_relaxed);
    size_t nextBuffer = scanBuffer ^ 1U;
#if defined(MRT_GC_UNIT_TESTS)
    flipTouchAccountingActive = true;
#endif
    // As in ZRememberedSet::flip (zRememberedSet.cpp:34-38), the STW
    // operation only publishes the other face. ScanPreviousForMinor is the
    // owner of consuming and clearing the face selected before this flip.
    activeBuffer.store(static_cast<uint8_t>(nextBuffer), std::memory_order_release);
#if defined(MRT_GC_UNIT_TESTS)
    flipTouchAccountingActive = false;
#endif
}

size_t RememberedSet::ScanPreviousForMinor(std::unordered_set<MAddress>& records)
{
    CheckInitialized();
    CHECK_DETAIL(records.empty(), "minor remembered-set destination must be empty");
    size_t scanBuffer = activeBuffer.load(std::memory_order_acquire) ^ 1U;
    const size_t expectedRecords = recordCounts[scanBuffer].load(std::memory_order_relaxed);
    records.reserve(expectedRecords);

    auto shouldScanPage = [](MAddress slot) -> bool {
        ZForwarding* forwarding = generation_forwarding_table(Generation::Old).get(slot);
        if (forwarding == nullptr) {
            return true;
        }
        return !forwarding->relocated_remembered_fields_is_concurrently_scanned();
    };

    size_t consumed = 0;
    VisitRememberedPages(scanBuffer, [&](size_t page) {
        const size_t firstWord = page * ZPage::UNIT_SIZE / (kBitsPerWord * kFieldBytes);
        const size_t endWord = std::min(wordCount,
            (page + 1) * ZPage::UNIT_SIZE / (kBitsPerWord * kFieldBytes));
        const MAddress pageStart = heapStart + page * ZPage::UNIT_SIZE;
        if (!shouldScanPage(pageStart)) {
            return;
        }
        for (size_t wordIdx = firstWord; wordIdx < endWord; ++wordIdx) {
            uint64_t word = bitmaps[scanBuffer][wordIdx].exchange(0, std::memory_order_relaxed);
            while (word != 0) {
                const unsigned bitInWord = static_cast<unsigned>(__builtin_ctzll(word));
                const size_t bit = wordIdx * kBitsPerWord + bitInWord;
                if (bit < bitCount) {
                    records.insert(heapStart + bit * kFieldBytes);
                    ++consumed;
                }
                word &= word - 1;
            }
        }
        rememberedPages[scanBuffer][page / kBitsPerWord].fetch_and(
            ~(uint64_t{1} << (page % kBitsPerWord)), std::memory_order_relaxed);
    });
    size_t remaining = expectedRecords > consumed ? expectedRecords - consumed : 0;
    recordCounts[scanBuffer].store(remaining, std::memory_order_relaxed);
    CHECK_DETAIL(consumed == records.size(), "remembered-set count mismatch: bitmap=%zu records=%zu", consumed,
                 records.size());

#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    std::lock_guard<std::mutex> guard(oracleLock);
    bool injected = false;
    bool equivalent = records.size() == oracleRecords[scanBuffer].size();
    for (MAddress slot : records) {
        equivalent = equivalent && oracleRecords[scanBuffer].count(slot) != 0;
    }
    if (!equivalent) {
        std::fprintf(stderr,
                     "REMSET_BITMAP_CROSSCHECK_MISMATCH operation=drain injected=%u bitmap=%zu oracle=%zu\n",
                     static_cast<unsigned>(injected), records.size(), oracleRecords[scanBuffer].size());
        std::abort();
    }
    lastDrainedHeapRecords = records.size();
    oracleRecords[scanBuffer].clear();
    ++bitmapCrossCheckCount;
#endif
    return records.size();
}

void RememberedSet::VisitPreviousInRange(MAddress start, size_t size,
                                         const std::function<void(MAddress)>& visitor) const
{
    if (!initialized || visitor == nullptr || size < kFieldBytes) {
        return;
    }
    MAddress end = start + size;
    if (start < heapStart) {
        start = heapStart;
    }
    if (end > heapStart + heapSize) {
        end = heapStart + heapSize;
    }
    if (start >= end) {
        return;
    }
    size_t firstBit = (start - heapStart + kFieldBytes - 1) / kFieldBytes;
    size_t endBit = (end - heapStart) / kFieldBytes;
    if (firstBit >= endBit || firstBit >= bitCount) {
        return;
    }
    if (endBit > bitCount) {
        endBit = bitCount;
    }
    const size_t previous = activeBuffer.load(std::memory_order_acquire) ^ 1U;
    for (size_t bit = firstBit; bit < endBit; ++bit) {
        size_t word = bit / kBitsPerWord;
        uint64_t mask = static_cast<uint64_t>(1) << (bit % kBitsPerWord);
        uint64_t w = bitmaps[previous][word].load(std::memory_order_relaxed);
        if ((w & mask) == 0) {
            continue;
        }
        visitor(heapStart + bit * kFieldBytes);
    }
}

size_t RememberedSet::DrainForMinor(std::unordered_set<MAddress>& records)
{
    FlipForMinor();
    return ScanPreviousForMinor(records);
}

std::unordered_set<MAddress> RememberedSet::Snapshot() const
{
    CheckInitialized();
    std::unordered_set<MAddress> records;
    size_t buffer = activeBuffer.load(std::memory_order_acquire);
    records.reserve(recordCounts[buffer].load(std::memory_order_relaxed));
    VisitRememberedPages(buffer, [&](size_t page) {
        const size_t firstWord = page * ZPage::UNIT_SIZE / (kBitsPerWord * kFieldBytes);
        const size_t endWord = std::min(wordCount,
            (page + 1) * ZPage::UNIT_SIZE / (kBitsPerWord * kFieldBytes));
        for (size_t wordIdx = firstWord; wordIdx < endWord; ++wordIdx) {
            uint64_t word = bitmaps[buffer][wordIdx].load(std::memory_order_relaxed);
            while (word != 0) {
                const unsigned bitInWord = static_cast<unsigned>(__builtin_ctzll(word));
                const size_t bit = wordIdx * kBitsPerWord + bitInWord;
                if (bit < bitCount) {
                    records.insert(heapStart + bit * kFieldBytes);
                }
                word &= word - 1;
            }
        }
    });
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    std::lock_guard<std::mutex> guard(oracleLock);
    size_t heapRecordCount = 0;
    bool equivalent = true;
    for (MAddress slot : records) {
        if (slot < heapStart || slot >= heapStart + heapSize) {
            continue;
        }
        ++heapRecordCount;
        equivalent = equivalent && oracleRecords[buffer].count(slot) != 0;
    }
    equivalent = equivalent && heapRecordCount == oracleRecords[buffer].size();
    if (!equivalent) {
        std::fprintf(stderr, "REMSET_BITMAP_CROSSCHECK_MISMATCH operation=snapshot bitmap=%zu oracle=%zu\n",
                     heapRecordCount, oracleRecords[buffer].size());
        std::abort();
    }
#endif
    return records;
}

bool RememberedSet::Contains(MAddress fieldAddress) const
{
    CheckInitialized();
    if (fieldAddress < heapStart || fieldAddress >= heapStart + heapSize) {
        return false;
    }
    size_t bit = AddressToBit(fieldAddress);
    size_t buffer = activeBuffer.load(std::memory_order_acquire);
    uint64_t word = bitmaps[buffer][bit / kBitsPerWord].load(std::memory_order_relaxed);
    return (word & (static_cast<uint64_t>(1) << (bit % kBitsPerWord))) != 0;
}

// zRememberedSet::was_remembered and verify_remset_cleared_{current,previous}.
bool RememberedSet::ContainsPrevious(MAddress fieldAddress) const
{
    CheckInitialized();
    if (fieldAddress < heapStart || fieldAddress >= heapStart + heapSize) { return false; }
    const size_t bit = AddressToBit(fieldAddress);
    const size_t previous = activeBuffer.load(std::memory_order_acquire) ^ 1U;
    const uint64_t word = bitmaps[previous][bit / kBitsPerWord].load(std::memory_order_relaxed);
    return (word & (uint64_t(1) << (bit % kBitsPerWord))) != 0;
}

bool RememberedSet::IsClearInRange(MAddress start, size_t size, bool current) const
{
    CheckInitialized();
    for (MAddress field = start; field < start + size; field += kFieldBytes) {
        if (current ? Contains(field) : ContainsPrevious(field)) { return false; }
    }
    return true;
}

size_t RememberedSet::Size() const
{
    CheckInitialized();
    size_t buffer = activeBuffer.load(std::memory_order_acquire);
    return recordCounts[buffer].load(std::memory_order_relaxed);
}

size_t RememberedSet::ClearRangeInBuffer(size_t buffer, size_t firstBit, size_t endBit, size_t* outWords)
{
    if (firstBit >= endBit) {
        return 0;
    }
    size_t firstWord = firstBit / kBitsPerWord;
    size_t lastWord = (endBit - 1) / kBitsPerWord;
    size_t removed = 0;
    for (size_t wordIdx = firstWord; wordIdx <= lastWord; ++wordIdx) {
        size_t first = wordIdx == firstWord ? firstBit % kBitsPerWord : 0;
        size_t last = wordIdx == lastWord ? (endBit - 1) % kBitsPerWord + 1 : kBitsPerWord;
        uint64_t lowMask = first == 0 ? 0 : (static_cast<uint64_t>(1) << first) - 1;
        uint64_t highMask = last == kBitsPerWord ? ~static_cast<uint64_t>(0) :
                                                       (static_cast<uint64_t>(1) << last) - 1;
        uint64_t mask = highMask & ~lowMask;
        uint64_t old = bitmaps[buffer][wordIdx].fetch_and(~mask, std::memory_order_relaxed);
        uint64_t cleared = old & mask;
        removed += static_cast<size_t>(__builtin_popcountll(cleared));
    }
    if (removed != 0) {
        recordCounts[buffer].fetch_sub(removed, std::memory_order_relaxed);
    }
    if (outWords != nullptr) {
        *outWords += lastWord - firstWord + 1;
    }
    return removed;
}

size_t RememberedSet::ClearRegion(MAddress start, MAddress end, size_t* outWords)
{
    CheckInitialized();
    if (outWords != nullptr) {
        *outWords = 0;
    }
    if (start >= end) {
        return 0;
    }
    CHECK_DETAIL(start >= heapStart && end <= heapStart + heapSize,
                 "remembered-set region [%#zx, %#zx) outside heap [%#zx, %#zx)", start, end, heapStart,
                 heapStart + heapSize);
    CHECK_DETAIL((start - heapStart) % kFieldBytes == 0 && (end - heapStart) % kFieldBytes == 0,
                 "remembered-set region [%#zx, %#zx) is not field-aligned", start, end);
    size_t firstBit = (start - heapStart) / kFieldBytes;
    size_t endBit = (end - heapStart) / kFieldBytes;
    size_t removed = 0;
    for (size_t buffer = 0; buffer < kBufferCount; ++buffer) {
        removed += ClearRangeInBuffer(buffer, firstBit, endBit, outWords);
    }
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    std::lock_guard<std::mutex> guard(oracleLock);
    for (size_t buffer = 0; buffer < kBufferCount; ++buffer) {
        for (auto it = oracleRecords[buffer].begin(); it != oracleRecords[buffer].end();) {
            if (*it >= start && *it < end) {
                it = oracleRecords[buffer].erase(it);
            } else {
                ++it;
            }
        }
    }
    ++bitmapCrossCheckCount;
#endif
    return removed;
}

size_t RememberedSet::ClearBuffer(size_t buffer)
{
    size_t removed = 0;
    VisitRememberedPages(buffer, [&](size_t page) {
#if defined(MRT_GC_UNIT_TESTS)
        if (flipTouchAccountingActive) {
            ++flipDirtyWordTouches;
        }
#endif
        const size_t firstWord = page * ZPage::UNIT_SIZE / (kBitsPerWord * kFieldBytes);
        const size_t endWord = std::min(wordCount,
            (page + 1) * ZPage::UNIT_SIZE / (kBitsPerWord * kFieldBytes));
        for (size_t word = firstWord; word < endWord; ++word) {
#if defined(MRT_GC_UNIT_TESTS)
            if (flipTouchAccountingActive) {
                ++flipBitmapWordTouches;
            }
#endif
            removed += static_cast<size_t>(
                __builtin_popcountll(bitmaps[buffer][word].exchange(0, std::memory_order_relaxed)));
        }
    });
    for (size_t word = 0; word < pageMapWordCount; ++word) {
        rememberedPages[buffer][word].store(0, std::memory_order_relaxed);
    }
    size_t expected = recordCounts[buffer].exchange(0, std::memory_order_relaxed);
    CHECK_DETAIL(removed == expected, "remembered-set page index mismatch: bitmap=%zu count=%zu", removed, expected);
    return removed;
}

uint8_t RememberedSet::BeginFullClear()
{
    CheckInitialized();
    size_t scanBuffer = activeBuffer.load(std::memory_order_acquire);
    size_t nextBuffer = scanBuffer ^ 1U;
    CHECK_DETAIL(ClearBuffer(nextBuffer) == 0, "remembered-set next full buffer is not empty");
    size_t previous = activeBuffer.exchange(static_cast<uint8_t>(nextBuffer), std::memory_order_acq_rel);
    CHECK_DETAIL(previous == scanBuffer, "concurrent remembered-set full rotation");
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    {
        std::lock_guard<std::mutex> guard(oracleLock);
        oracleRecords[nextBuffer].clear();
    }
#endif
    return static_cast<uint8_t>(scanBuffer);
}

size_t RememberedSet::FinishFullClear(uint8_t scanBuffer)
{
    CheckInitialized();
    CHECK_DETAIL(scanBuffer < kBufferCount, "invalid remembered-set scan buffer %u", scanBuffer);
    CHECK_DETAIL(scanBuffer != activeBuffer.load(std::memory_order_acquire),
                 "cannot clear active remembered-set buffer");
    size_t heapRemoved = ClearBuffer(scanBuffer);
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    std::lock_guard<std::mutex> guard(oracleLock);
    CHECK_DETAIL(heapRemoved == oracleRecords[scanBuffer].size(),
                 "full remembered-set cross-check mismatch: bitmap=%zu oracle=%zu", heapRemoved,
                 oracleRecords[scanBuffer].size());
    oracleRecords[scanBuffer].clear();
    ++bitmapCrossCheckCount;
#endif
    return heapRemoved;
}

#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
void RememberedSet::RecordStaticForCrossCheck(MAddress fieldAddress, MAddress callsite)
{
    std::lock_guard<std::mutex> guard(oracleLock);
    staticRecords.insert(fieldAddress);
    staticRecordSites[fieldAddress] = callsite;
}

void RememberedSet::VisitStaticForCrossCheck(MAddress fieldAddress)
{
    std::lock_guard<std::mutex> guard(oracleLock);
    visitedStaticRoots.insert(fieldAddress);
}

void RememberedSet::CheckStaticCoverageForMinor()
{
    std::lock_guard<std::mutex> guard(oracleLock);
    staticRecords.clear();
    staticRecordSites.clear();
    visitedStaticRoots.clear();
    lastDrainedHeapRecords = 0;
}
#endif
} // namespace MapleRuntime

#include "Heap/z/zRememberedSet.inline.hpp"

namespace MapleRuntime {
bool RememberedSet::IsInitialized() const { return initialized; }
}
