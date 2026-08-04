// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Barrier/RememberedSet.h"

#include <new>
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif

#include "Base/Log.h"

namespace MapleRuntime {
void RememberedSet::Initialize(MAddress start, size_t size)
{
    CHECK_DETAIL(!initialized, "remembered set initialized twice");
    CHECK_DETAIL(size != 0, "remembered set heap range is empty");
    CHECK_DETAIL(start % kFieldBytes == 0, "remembered set heap start %#zx is not field-aligned", start);
    heapStart = start;
    heapSize = size;
    bitCount = (size + kFieldBytes - 1) / kFieldBytes;
    wordCount = (bitCount + kBitsPerWord - 1) / kBitsPerWord;
    dirtyWordCount = (wordCount + kBitsPerWord - 1) / kBitsPerWord;
    for (size_t buffer = 0; buffer < kBufferCount; ++buffer) {
        bitmaps[buffer].reset(new (std::nothrow) std::atomic<uint64_t>[wordCount]);
        CHECK_DETAIL(bitmaps[buffer] != nullptr, "failed to allocate remembered-set bitmap");
        for (size_t word = 0; word < wordCount; ++word) {
            bitmaps[buffer][word].store(0, std::memory_order_relaxed);
        }
        dirtyMaps[buffer].reset(new (std::nothrow) std::atomic<uint64_t>[dirtyWordCount]);
        CHECK_DETAIL(dirtyMaps[buffer] != nullptr, "failed to allocate remembered-set dirty map");
        for (size_t word = 0; word < dirtyWordCount; ++word) {
            dirtyMaps[buffer][word].store(0, std::memory_order_relaxed);
        }
    }
    initialized = true;
}

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

void RememberedSet::MarkWordDirty(size_t buffer, size_t word)
{
    size_t dirtyWord = word / kBitsPerWord;
    uint64_t mask = static_cast<uint64_t>(1) << (word % kBitsPerWord);
    dirtyMaps[buffer][dirtyWord].fetch_or(mask, std::memory_order_relaxed);
}

void RememberedSet::ClearWordDirty(size_t buffer, size_t word)
{
    size_t dirtyWord = word / kBitsPerWord;
    uint64_t mask = static_cast<uint64_t>(1) << (word % kBitsPerWord);
    dirtyMaps[buffer][dirtyWord].fetch_and(~mask, std::memory_order_relaxed);
}

void RememberedSet::Record(MAddress fieldAddress)
{
    CheckInitialized();
    size_t bit = AddressToBit(fieldAddress);
    size_t word = bit / kBitsPerWord;
    uint64_t mask = static_cast<uint64_t>(1) << (bit % kBitsPerWord);
    size_t buffer = activeBuffer.load(std::memory_order_acquire);
    uint64_t old = bitmaps[buffer][word].fetch_or(mask, std::memory_order_relaxed);
    MarkWordDirty(buffer, word);
    if ((old & mask) == 0) {
        recordCounts[buffer].fetch_add(1, std::memory_order_relaxed);
    }
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    std::lock_guard<std::mutex> guard(oracleLock);
    oracleRecords[buffer].insert(fieldAddress);
#endif
}

size_t RememberedSet::DrainForMinor(std::unordered_set<MAddress>& records)
{
    CheckInitialized();
    CHECK_DETAIL(records.empty(), "minor remembered-set destination must be empty");

    // DoYoungGarbageCollection owns a ScopedStopTheWorld across this operation.
    // GC workers start rebuilding records only after this synchronous drain returns.
    size_t scanBuffer = activeBuffer.load(std::memory_order_relaxed);
    size_t nextBuffer = scanBuffer ^ 1U;
    CHECK_DETAIL(ClearBuffer(nextBuffer) == 0 && recordCounts[nextBuffer].load(std::memory_order_relaxed) == 0,
                 "remembered-set next buffer is not empty at minor swap");
    activeBuffer.store(static_cast<uint8_t>(nextBuffer), std::memory_order_release);

    for (size_t dirtyIdx = 0; dirtyIdx < dirtyWordCount; ++dirtyIdx) {
        uint64_t dirty = dirtyMaps[scanBuffer][dirtyIdx].exchange(0, std::memory_order_relaxed);
        while (dirty != 0) {
            unsigned wordInDirty = static_cast<unsigned>(__builtin_ctzll(dirty));
            size_t wordIdx = dirtyIdx * kBitsPerWord + wordInDirty;
            if (wordIdx < wordCount) {
                uint64_t word = bitmaps[scanBuffer][wordIdx].exchange(0, std::memory_order_relaxed);
                while (word != 0) {
                    unsigned bitInWord = static_cast<unsigned>(__builtin_ctzll(word));
                    size_t bit = wordIdx * kBitsPerWord + bitInWord;
                    if (bit < bitCount) {
                        records.insert(heapStart + bit * kFieldBytes);
                    }
                    word &= word - 1;
                }
            }
            dirty &= dirty - 1;
        }
    }
    size_t recorded = recordCounts[scanBuffer].exchange(0, std::memory_order_relaxed);
    CHECK_DETAIL(recorded == records.size(), "remembered-set count mismatch: bitmap=%zu records=%zu", recorded,
                 records.size());

#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    std::lock_guard<std::mutex> guard(oracleLock);
    bool injected = false;
    const char* inject = std::getenv("MRT_GCV2_VERIFY_REMSET_BITMAP_INJECT_MISMATCH");
    if (inject != nullptr && std::strcmp(inject, "1") == 0) {
        for (size_t bit = 0; bit < bitCount; ++bit) {
            MAddress candidate = heapStart + bit * kFieldBytes;
            if (oracleRecords[scanBuffer].count(candidate) == 0) {
                oracleRecords[scanBuffer].insert(candidate);
                injected = true;
                break;
            }
        }
    }
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
    return recorded;
}

std::unordered_set<MAddress> RememberedSet::Snapshot() const
{
    CheckInitialized();
    std::unordered_set<MAddress> records;
    size_t buffer = activeBuffer.load(std::memory_order_acquire);
    records.reserve(recordCounts[buffer].load(std::memory_order_relaxed));
    for (size_t dirtyIdx = 0; dirtyIdx < dirtyWordCount; ++dirtyIdx) {
        uint64_t dirty = dirtyMaps[buffer][dirtyIdx].load(std::memory_order_relaxed);
        while (dirty != 0) {
            unsigned wordInDirty = static_cast<unsigned>(__builtin_ctzll(dirty));
            size_t wordIdx = dirtyIdx * kBitsPerWord + wordInDirty;
            if (wordIdx < wordCount) {
                uint64_t word = bitmaps[buffer][wordIdx].load(std::memory_order_relaxed);
                while (word != 0) {
                    unsigned bitInWord = static_cast<unsigned>(__builtin_ctzll(word));
                    size_t bit = wordIdx * kBitsPerWord + bitInWord;
                    if (bit < bitCount) {
                        records.insert(heapStart + bit * kFieldBytes);
                    }
                    word &= word - 1;
                }
            }
            dirty &= dirty - 1;
        }
    }
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    std::lock_guard<std::mutex> guard(oracleLock);
    bool equivalent = records.size() == oracleRecords[buffer].size();
    for (MAddress slot : records) {
        equivalent = equivalent && oracleRecords[buffer].count(slot) != 0;
    }
    if (!equivalent) {
        std::fprintf(stderr, "REMSET_BITMAP_CROSSCHECK_MISMATCH operation=snapshot bitmap=%zu oracle=%zu\n",
                     records.size(), oracleRecords[buffer].size());
        std::abort();
    }
#endif
    return records;
}

bool RememberedSet::Contains(MAddress fieldAddress) const
{
    CheckInitialized();
    size_t bit = AddressToBit(fieldAddress);
    size_t buffer = activeBuffer.load(std::memory_order_acquire);
    uint64_t word = bitmaps[buffer][bit / kBitsPerWord].load(std::memory_order_relaxed);
    return (word & (static_cast<uint64_t>(1) << (bit % kBitsPerWord))) != 0;
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
        removed += static_cast<size_t>(__builtin_popcountll(old & mask));
        if ((old & ~mask) == 0) {
            ClearWordDirty(buffer, wordIdx);
        }
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
    for (size_t dirtyIdx = 0; dirtyIdx < dirtyWordCount; ++dirtyIdx) {
        uint64_t dirty = dirtyMaps[buffer][dirtyIdx].exchange(0, std::memory_order_relaxed);
        while (dirty != 0) {
            unsigned wordInDirty = static_cast<unsigned>(__builtin_ctzll(dirty));
            size_t word = dirtyIdx * kBitsPerWord + wordInDirty;
            if (word < wordCount) {
                removed += static_cast<size_t>(
                    __builtin_popcountll(bitmaps[buffer][word].exchange(0, std::memory_order_relaxed)));
            }
            dirty &= dirty - 1;
        }
    }
    size_t expected = recordCounts[buffer].exchange(0, std::memory_order_relaxed);
    CHECK_DETAIL(removed == expected, "remembered-set dirty index mismatch: bitmap=%zu count=%zu", removed, expected);
    return removed;
}

uint8_t RememberedSet::BeginFullClear()
{
    CheckInitialized();
    size_t scanBuffer = activeBuffer.load(std::memory_order_acquire);
    size_t nextBuffer = scanBuffer ^ 1U;
    CHECK_DETAIL(ClearBuffer(nextBuffer) == 0, "remembered-set next full buffer is not empty");
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    {
        std::lock_guard<std::mutex> guard(oracleLock);
        oracleRecords[nextBuffer].clear();
    }
#endif
    size_t previous = activeBuffer.exchange(static_cast<uint8_t>(nextBuffer), std::memory_order_acq_rel);
    CHECK_DETAIL(previous == scanBuffer, "concurrent remembered-set full rotation");
    return static_cast<uint8_t>(scanBuffer);
}

size_t RememberedSet::FinishFullClear(uint8_t scanBuffer)
{
    CheckInitialized();
    CHECK_DETAIL(scanBuffer < kBufferCount, "invalid remembered-set scan buffer %u", scanBuffer);
    CHECK_DETAIL(scanBuffer != activeBuffer.load(std::memory_order_acquire),
                 "cannot clear active remembered-set buffer");
    size_t removed = ClearBuffer(scanBuffer);
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    std::lock_guard<std::mutex> guard(oracleLock);
    CHECK_DETAIL(removed == oracleRecords[scanBuffer].size(),
                 "full remembered-set cross-check mismatch: bitmap=%zu oracle=%zu", removed,
                 oracleRecords[scanBuffer].size());
    oracleRecords[scanBuffer].clear();
    ++bitmapCrossCheckCount;
#endif
    return removed;
}

#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
void RememberedSet::RecordStaticForCrossCheck(MAddress fieldAddress)
{
    std::lock_guard<std::mutex> guard(oracleLock);
    staticRecords.insert(fieldAddress);
}

void RememberedSet::VisitStaticForCrossCheck(MAddress fieldAddress)
{
    std::lock_guard<std::mutex> guard(oracleLock);
    visitedStaticRoots.insert(fieldAddress);
}

void RememberedSet::CheckStaticCoverageForMinor()
{
    std::lock_guard<std::mutex> guard(oracleLock);
    bool injected = false;
    const char* inject = std::getenv("MRT_GCV2_VERIFY_STATIC_COVERAGE_INJECT_MISSING");
    if (inject != nullptr && std::strcmp(inject, "1") == 0) {
        staticRecords.insert(heapStart);
        injected = true;
    }
    size_t missing = 0;
    for (MAddress slot : staticRecords) {
        missing += visitedStaticRoots.count(slot) == 0 ? 1 : 0;
    }
    ++staticCrossCheckRounds;
    size_t legacyTotal = lastDrainedHeapRecords + staticRecords.size();
    double removedShare = legacyTotal == 0 ? 0.0 :
        100.0 * static_cast<double>(staticRecords.size()) / static_cast<double>(legacyTotal);
    std::fprintf(stderr, "REMSET_MINOR_ROOT_SCAN round=%zu fired=1\n", staticCrossCheckRounds);
    std::fprintf(stderr,
                 "REMSET_STATIC_COVERAGE round=%zu recorded=%zu visited=%zu missing=%zu injected=%u "
                 "heap_records=%zu legacy_total=%zu removed_share=%.6f%%\n",
                 staticCrossCheckRounds, staticRecords.size(), visitedStaticRoots.size(), missing,
                 static_cast<unsigned>(injected), lastDrainedHeapRecords, legacyTotal, removedShare);
    if (missing != 0) {
        std::fprintf(stderr, "REMSET_STATIC_COVERAGE_MISMATCH injected=%u missing=%zu recorded=%zu\n",
                     static_cast<unsigned>(injected), missing, staticRecords.size());
        std::abort();
    }
    staticRecords.clear();
    visitedStaticRoots.clear();
    lastDrainedHeapRecords = 0;
}
#endif
} // namespace MapleRuntime
