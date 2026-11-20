// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#include "Heap/z/zJNICritical.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>
#include "Base/Log.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zLock.inline.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zRelocationSetSelector.inline.hpp"
#include "Heap/z/zStat.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadSMR.h"

namespace MapleRuntime {
namespace {
const ZStatCriticalPhase ZCriticalPhaseJNICriticalStall("JNI Critical Stall");

// Pages selected for relocation in one cycle of one generation, sorted. Immutable
// once published.
struct ZRawDataSelection {
    uint64_t epoch;
    std::vector<ZPage*> pages;
    // Set by the final scan right before the relocate-start pause (finish_selection).
    std::atomic<bool> finalScan{ false };
    bool contains(const ZPage* page) const { return std::binary_search(pages.begin(), pages.end(), page); }
};

constexpr size_t kGenerations = 2;
size_t GenerationIndex(ZGenerationId id) { return id == ZGenerationId::young ? 0 : 1; }

std::atomic<ZRawDataSelection*> openSelection[kGenerations]{};
// A closed selection is freed when the next selection of the same generation
// opens. An acquirer dereferences a selection only in managed state, between two
// safepoint polls, and at least the next cycle's mark-start pause lies between a
// close and the next open of the same generation, so no reader can still use it.
ZRawDataSelection* retiredSelection[kGenerations]{};
std::atomic<uint64_t> openedEpoch[kGenerations]{};
std::atomic<uint64_t> closedEpoch[kGenerations]{};
std::atomic<uint64_t> retreatCount{ 0 };
std::atomic<uint64_t> deselectedCount{ 0 };
std::atomic<uint64_t> lateDroppedCount{ 0 };
// Acquirers that met an open selection wait here (in saferegion) for it to close.
ZConditionLock* lock = nullptr;

ZPage* PageOf(uintptr_t array) { return array == 0 ? nullptr : Heap::page(static_cast<MAddress>(array)); }

// A hold in a strictly lower slot of the same mutator on `page`. ScanHolds reads
// rawDataOverflow and then slots in ascending order, so only a slot it reads
// before `self` can cover this acquire. If that lower slot is already clear when
// the scan loads it, this hold was stored earlier on the same thread (the cover
// was still present when this slot was published) and still sits in a slot the
// scan has not read. A higher slot cannot cover: the scan may already have loaded
// this slot before the higher one is cleared. An overflow hold (self < 0) is read
// before every slot, so the loop covers nothing.
bool CoveredByEarlierHold(const Mutator& mutator, int self, const ZPage* page)
{
    for (int i = 0; i < self; ++i) {
        const uintptr_t held = mutator.rawDataSlots[i].load(std::memory_order_relaxed);
        if (held != 0 && PageOf(held) == page) {
            return true;
        }
    }
    return false;
}

// Selected pages of `selection` that hold an array of some mutator (sorted,
// unique); `overflow` if some mutator holds more arrays than it has slots for.
// The load order is load-bearing for CoveredByEarlierHold: rawDataOverflow,
// then slots 0 .. kRawDataSlots-1. A cover is only a lower slot.
void ScanHolds(const ZRawDataSelection& selection, std::vector<ZPage*>& held, bool& overflow)
{
    overflow = false;
    // HotSpot threadSMR: a stable list of every registered mutator, usable from
    // the collector thread without registration locks (as zStackWatermark.cpp).
    ThreadsListHandle threads;
    for (size_t t = 0; t < threads.length(); ++t) {
        const Mutator& mutator = *threads.thread_at(t);
        if (mutator.rawDataOverflow.load(std::memory_order_seq_cst) != 0) {
            overflow = true;
        }
        for (int i = 0; i < Mutator::kRawDataSlots; ++i) {
            ZPage* const page = PageOf(mutator.rawDataSlots[i].load(std::memory_order_seq_cst));
            if (page != nullptr && selection.contains(page)) {
                held.push_back(page);
            }
        }
    }
    std::sort(held.begin(), held.end());
    held.erase(std::unique(held.begin(), held.end()), held.end());
}

// Publishes `pages` as the open selection of generation `g`, then scans every
// mutator's holds. Returns the selected pages that hold an array (sorted, unique)
// and whether some mutator holds more arrays than it has slots for.
ZRawDataSelection* Publish(size_t g, std::vector<ZPage*>&& pages, std::vector<ZPage*>& held, bool& overflow)
{
    CHECK_DETAIL(openSelection[g].load(std::memory_order_relaxed) == nullptr, "Raw-data selection already open");
    delete retiredSelection[g];
    retiredSelection[g] = nullptr;
    const uint64_t epoch = openedEpoch[g].fetch_add(1, std::memory_order_relaxed) + 1;
    overflow = false;
    if (pages.empty()) {
        return nullptr;
    }
    std::sort(pages.begin(), pages.end());
    auto* selection = new ZRawDataSelection{ epoch, std::move(pages) };
    openSelection[g].store(selection, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ScanHolds(*selection, held, overflow);
    return selection;
}
} // namespace

void ZJNICritical::initialize()
{
    // A runtime may be initialized again in the same process (unit tests): no
    // selection survives a finished runtime.
    for (size_t g = 0; g < kGenerations; ++g) {
        delete openSelection[g].exchange(nullptr, std::memory_order_relaxed);
        delete retiredSelection[g];
        retiredSelection[g] = nullptr;
        closedEpoch[g].store(openedEpoch[g].load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    if (lock == nullptr) {
        lock = new ZConditionLock();
    }
}

bool ZJNICritical::enter(Mutator& mutator, uintptr_t array)
{
    int slot = -1;
    for (int i = 0; i < Mutator::kRawDataSlots; ++i) {
        if (mutator.rawDataSlots[i].load(std::memory_order_relaxed) == 0) {
            slot = i;
            break;
        }
    }
    // seq_cst store, then seq_cst loads of the selections: pairs with the
    // collector's publication store and its seq_cst scan (Publish/finish_selection).
    if (slot >= 0) {
        mutator.rawDataSlots[slot].store(array, std::memory_order_seq_cst);
    } else {
        mutator.rawDataOverflow.fetch_add(1, std::memory_order_seq_cst);
    }
    ZRawDataSelection* conflict = nullptr;
    size_t generation = 0;
    ZPage* page = nullptr;
    for (size_t g = 0; g < kGenerations && conflict == nullptr; ++g) {
        ZRawDataSelection* const selection = openSelection[g].load(std::memory_order_seq_cst);
        if (selection == nullptr) {
            continue;
        }
        if (page == nullptr) {
            page = PageOf(array);
            if (page == nullptr) {
                break;
            }
        }
        if (selection->contains(page) && !CoveredByEarlierHold(mutator, slot, page)) {
            if (!selection->finalScan.load(std::memory_order_seq_cst)) {
                // The final scan before the pause sees this slot and drops the page.
                continue;
            }
            conflict = selection;
            generation = g;
        }
    }
    if (conflict == nullptr) {
        return true;
    }
    // The page may be relocated in this cycle: do not hold it now. Wait until the
    // relocate-start pause of that cycle has passed; the reloaded array is then
    // either in place or in a to-space page that this cycle no longer moves.
    const uint64_t epoch = conflict->epoch;
    if (slot >= 0) {
        mutator.rawDataSlots[slot].store(0, std::memory_order_release);
    } else {
        mutator.rawDataOverflow.fetch_sub(1, std::memory_order_release);
    }
    retreatCount.fetch_add(1, std::memory_order_relaxed);
    ZStatTimer timer(ZCriticalPhaseJNICriticalStall);
    ScopedEnterSaferegion enterSaferegion(true);
    ZLocker<ZConditionLock> guard(lock);
    while (closedEpoch[generation].load(std::memory_order_acquire) < epoch) {
        lock->wait();
    }
    return false;
}

void ZJNICritical::exit(Mutator& mutator, uintptr_t array)
{
    for (int i = Mutator::kRawDataSlots - 1; i >= 0; --i) {
        if (mutator.rawDataSlots[i].load(std::memory_order_relaxed) == array) {
            mutator.rawDataSlots[i].store(0, std::memory_order_release);
            return;
        }
    }
    if (mutator.rawDataOverflow.load(std::memory_order_relaxed) != 0) {
        mutator.rawDataOverflow.fetch_sub(1, std::memory_order_release);
        return;
    }
    // Protection is per page: dropping another hold of this mutator on the same
    // page leaves that page exactly as protected as before.
    const ZPage* const page = PageOf(array);
    for (int i = Mutator::kRawDataSlots - 1; i >= 0; --i) {
        const uintptr_t held = mutator.rawDataSlots[i].load(std::memory_order_relaxed);
        if (held != 0 && PageOf(held) == page) {
            mutator.rawDataSlots[i].store(0, std::memory_order_release);
            return;
        }
    }
    CHECK_DETAIL(false, "Raw data released without a matching acquire: %p", reinterpret_cast<void*>(array));
}

size_t ZJNICritical::open_selection(ZGenerationId id, ZRelocationSetSelector* selector)
{
    std::vector<ZPage*> pages;
    for (const ZArray<ZPage*>* group : { selector->selected_small(), selector->selected_medium() }) {
        ZArrayIterator<ZPage*> iter(group);
        for (ZPage* page; iter.next(&page);) {
            pages.push_back(page);
        }
    }
    std::vector<ZPage*> held;
    bool overflow = false;
    if (Publish(GenerationIndex(id), std::move(pages), held, overflow) == nullptr) {
        return 0;
    }
    const size_t deselected = selector->deselect(held, overflow);
    deselectedCount.fetch_add(deselected, std::memory_order_relaxed);
    return deselected;
}

size_t ZJNICritical::open_selection_for_test(ZGenerationId id, ZPage* const* pages, size_t n)
{
    if (openSelection[GenerationIndex(id)].load(std::memory_order_acquire) != nullptr) {
        return SIZE_MAX;
    }
    std::vector<ZPage*> held;
    bool overflow = false;
    ZRawDataSelection* const selection = Publish(GenerationIndex(id), std::vector<ZPage*>(pages, pages + n), held, overflow);
    if (selection == nullptr) {
        return 0;
    }
    return overflow ? selection->pages.size() : held.size();
}

void ZJNICritical::finish_selection(ZGenerationId id, std::vector<ZPage*>& held, bool& all)
{
    held.clear();
    all = false;
    ZRawDataSelection* const selection = openSelection[GenerationIndex(id)].load(std::memory_order_acquire);
    if (selection == nullptr) {
        return;
    }
    selection->finalScan.store(true, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ScanHolds(*selection, held, all);
}

void ZJNICritical::note_late_dropped(size_t n) { lateDroppedCount.fetch_add(n, std::memory_order_relaxed); }

uint64_t ZJNICritical::late_dropped_pages() { return lateDroppedCount.load(std::memory_order_relaxed); }

void ZJNICritical::close_selection(ZGenerationId id)
{
    const size_t g = GenerationIndex(id);
    ZRawDataSelection* const selection = openSelection[g].exchange(nullptr, std::memory_order_seq_cst);
    if (selection == nullptr) {
        return;
    }
    delete retiredSelection[g];
    retiredSelection[g] = selection;
    ZLocker<ZConditionLock> guard(lock);
    closedEpoch[g].store(selection->epoch, std::memory_order_release);
    lock->notify_all();
}

int64_t ZJNICritical::holds(const Mutator& mutator)
{
    int64_t n = mutator.rawDataOverflow.load(std::memory_order_acquire);
    for (int i = 0; i < Mutator::kRawDataSlots; ++i) {
        n += mutator.rawDataSlots[i].load(std::memory_order_acquire) != 0 ? 1 : 0;
    }
    return n;
}

int64_t ZJNICritical::count_snapshot()
{
    int64_t holders = 0;
    ThreadsListHandle threads;
    for (size_t t = 0; t < threads.length(); ++t) {
        holders += holds(*threads.thread_at(t)) != 0 ? 1 : 0;
    }
    return holders;
}

bool ZJNICritical::selection_open(ZGenerationId id)
{
    return openSelection[GenerationIndex(id)].load(std::memory_order_acquire) != nullptr;
}

uint64_t ZJNICritical::closed_selections(ZGenerationId id)
{
    return closedEpoch[GenerationIndex(id)].load(std::memory_order_acquire);
}

uint64_t ZJNICritical::retreats() { return retreatCount.load(std::memory_order_relaxed); }

uint64_t ZJNICritical::deselected_pages() { return deselectedCount.load(std::memory_order_relaxed); }

} // namespace MapleRuntime
