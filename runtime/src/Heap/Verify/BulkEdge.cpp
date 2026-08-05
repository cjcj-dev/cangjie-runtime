// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "BulkEdge.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "Heap/Heap.h"
#include "Heap/Allocator/RegionSpace.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

size_t EnvAsSize(const char* name, size_t def)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return def;
    }
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v || n == 0) {
        return def;
    }
    return static_cast<size_t>(n);
}

// Open-addressed set of MAddress keys. Fixed cap; overflow drops (counted).
// Single-writer friendly; concurrent Note uses CAS on slots (0 = empty).
constexpr size_t kDefaultCap = 1u << 20; // 1M slots ≈ 8MB
std::atomic<uint64_t>* gTable = nullptr;
size_t gCap = 0;
std::atomic<size_t> gNoteCalls{0};
std::atomic<size_t> gInserted{0};
std::atomic<size_t> gDropped{0};
std::atomic<size_t> gContainsHits{0};
std::atomic<size_t> gContainsMisses{0};
std::atomic<int> gInited{0};

void EnsureInit()
{
    int expected = 0;
    if (!gInited.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        while (gInited.load(std::memory_order_acquire) != 2) {
        }
        return;
    }
    gCap = EnvAsSize("MRT_GCV2_BULKEDGE_CAP", kDefaultCap);
    if (gCap < 1024) {
        gCap = 1024;
    }
    // Power-of-two for mask hash.
    size_t cap = 1;
    while (cap < gCap) {
        cap <<= 1;
    }
    gCap = cap;
    gTable = new std::atomic<uint64_t>[gCap];
    for (size_t i = 0; i < gCap; ++i) {
        gTable[i].store(0, std::memory_order_relaxed);
    }
    gInited.store(2, std::memory_order_release);
    VLOG(REPORT, "[GCV2][BULKEDGE] ARMED env=MRT_GCV2_BULKEDGE=1 cap=%zu", gCap);
}

inline size_t Hash(uint64_t k)
{
    // SplitMix64 low bits.
    k ^= k >> 30;
    k *= 0xbf58476d1ce4e5b9ULL;
    k ^= k >> 27;
    k *= 0x94d049bb133111ebULL;
    k ^= k >> 31;
    return static_cast<size_t>(k);
}

bool Insert(uint64_t key)
{
    if (key == 0) {
        return false;
    }
    EnsureInit();
    size_t mask = gCap - 1;
    size_t i = Hash(key) & mask;
    for (size_t n = 0; n < 64; ++n) {
        size_t idx = (i + n) & mask;
        uint64_t cur = gTable[idx].load(std::memory_order_relaxed);
        if (cur == key) {
            return true;
        }
        if (cur == 0) {
            uint64_t expect = 0;
            if (gTable[idx].compare_exchange_strong(expect, key, std::memory_order_relaxed)) {
                gInserted.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            if (expect == key) {
                return true;
            }
        }
    }
    gDropped.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool Lookup(uint64_t key)
{
    if (key == 0 || gTable == nullptr) {
        return false;
    }
    size_t mask = gCap - 1;
    size_t i = Hash(key) & mask;
    for (size_t n = 0; n < 64; ++n) {
        size_t idx = (i + n) & mask;
        uint64_t cur = gTable[idx].load(std::memory_order_relaxed);
        if (cur == 0) {
            return false;
        }
        if (cur == key) {
            return true;
        }
    }
    return false;
}

} // namespace

bool BulkEdge::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_BULKEDGE");
    return on;
}

void BulkEdge::NoteBulkRange(MAddress start, size_t size, const char* site)
{
    if (!Enabled() || size == 0 || start == 0) {
        return;
    }
    gNoteCalls.fetch_add(1, std::memory_order_relaxed);
    // Align up to pointer boundary; step by sizeof(void*).
    MAddress aligned = (start + (sizeof(void*) - 1)) & ~(static_cast<MAddress>(sizeof(void*) - 1));
    MAddress end = start + size;
    size_t n = 0;
    for (MAddress p = aligned; p + sizeof(void*) <= end; p += sizeof(void*)) {
        Insert(static_cast<uint64_t>(p));
        ++n;
        // Soft cap per call to avoid huge struct copies dominating.
        if (n >= 4096) {
            break;
        }
    }
    (void)site;
}

bool BulkEdge::Contains(MAddress slot)
{
    if (!Enabled()) {
        return false;
    }
    bool hit = Lookup(static_cast<uint64_t>(slot));
    if (hit) {
        gContainsHits.fetch_add(1, std::memory_order_relaxed);
    } else {
        gContainsMisses.fetch_add(1, std::memory_order_relaxed);
    }
    return hit;
}

void BulkEdge::Stats(size_t& noteCalls, size_t& slotsInserted, size_t& slotsDropped, size_t& cap,
                     size_t& containsHits, size_t& containsMisses)
{
    noteCalls = gNoteCalls.load(std::memory_order_relaxed);
    slotsInserted = gInserted.load(std::memory_order_relaxed);
    slotsDropped = gDropped.load(std::memory_order_relaxed);
    cap = gCap;
    containsHits = gContainsHits.load(std::memory_order_relaxed);
    containsMisses = gContainsMisses.load(std::memory_order_relaxed);
}

size_t BulkEdge::ClassifyHolderInEdges(void* holder, int holderValid, int holderMarked)
{
    if (!Enabled() || holder == nullptr) {
        return 0;
    }
    BaseObject* holderObj = reinterpret_cast<BaseObject*>(holder);
    size_t edgeN = 0;
    size_t bulkN = 0;
    size_t nonBulkN = 0;
    BaseObject* sampleReferrer = nullptr;
    MAddress sampleSlot = 0;
    int sampleBulk = -1;
    int sampleRefMarked = -1;

    Heap::GetHeap().ForEachObj(
        [holderObj, &edgeN, &bulkN, &nonBulkN, &sampleReferrer, &sampleSlot, &sampleBulk, &sampleRefMarked](
            BaseObject* cand) {
            if (cand == nullptr || cand == holderObj) {
                return;
            }
            if (!cand->IsValidObject()) {
                return;
            }
            if (!cand->HasRefField()) {
                return;
            }
            cand->ForEachRefField([holderObj, cand, &edgeN, &bulkN, &nonBulkN, &sampleReferrer, &sampleSlot,
                                   &sampleBulk, &sampleRefMarked](RefField<>& rf) {
                BaseObject* tgt = rf.GetTargetObject();
                if (tgt != holderObj) {
                    return;
                }
                MAddress slot = reinterpret_cast<MAddress>(&rf);
                bool isBulk = Contains(slot);
                ++edgeN;
                if (isBulk) {
                    ++bulkN;
                } else {
                    ++nonBulkN;
                }
                if (sampleReferrer == nullptr) {
                    sampleReferrer = cand;
                    sampleSlot = slot;
                    sampleBulk = isBulk ? 1 : 0;
                    sampleRefMarked = static_cast<int>(RegionSpace::IsMarkedObject(cand));
                }
            });
        },
        false);

    size_t noteCalls = 0;
    size_t slotsInserted = 0;
    size_t slotsDropped = 0;
    size_t cap = 0;
    size_t ch = 0;
    size_t cm = 0;
    Stats(noteCalls, slotsInserted, slotsDropped, cap, ch, cm);

    const char* verdict = "BULKEDGE_NONE";
    if (edgeN == 0) {
        verdict = "BULKEDGE_NO_INEDGE";
    } else if (bulkN == edgeN) {
        verdict = "BULKEDGE_ALL_BULK";
    } else if (bulkN > 0) {
        verdict = "BULKEDGE_PARTIAL";
    } else {
        verdict = "BULKEDGE_NONE";
    }

    VLOG(REPORT,
         "[GCV2][BULKEDGE] holder=%p holderValid=%d holderMarked=%d "
         "inEdgeN=%zu bulkN=%zu nonBulkN=%zu verdict=%s "
         "sampleReferrer=%p sampleSlot=%p sampleBulk=%d sampleRefMarked=%d "
         "noteCalls=%zu slotsInserted=%zu slotsDropped=%zu cap=%zu containsHits=%zu containsMisses=%zu",
         holder, holderValid, holderMarked, edgeN, bulkN, nonBulkN, verdict, sampleReferrer,
         reinterpret_cast<void*>(sampleSlot), sampleBulk, sampleRefMarked, noteCalls, slotsInserted, slotsDropped, cap,
         ch, cm);

    return bulkN;
}

} // namespace MapleRuntime
