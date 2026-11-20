// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_JNI_CRITICAL_HPP
#define MRT_Z_JNI_CRITICAL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "Heap/z/zGenerationId.hpp"

namespace MapleRuntime {

class Mutator;
class ZPage;
class ZRelocationSetSelector;

// Raw-data holders (MCC_AcquireRawData .. MCC_ReleaseRawData).
//
// JDK ZGC blocks JNI critical regions around the mark-start and relocate-start
// pauses (zGeneration.cpp:425-485), so a holder that legally blocks in a foreign
// call stalls the collector. Here no pause and no collector phase waits for a
// holder. Each mutator records the decoded arrays it holds in its own slots
// (Mutator::rawDataSlots / rawDataOverflow); there is no process-wide holder table.
// The relocation-set selection of a generation keeps exactly the pages that
// contain a held array out of the relocation set (they take the existing
// not-selected path), every other page is selected normally.
//
// Per generation, once per cycle:
//   collector: select() -> publish the selected pages (open_selection, seq_cst)
//              -> fence -> scan every mutator's slots -> deselect the selected
//              pages that hold an array -> install ... -> final scan right
//              before the relocate-start pause (finish_selection: seq_cst flag,
//              fence, scan) -> drop the forwardings of the pages held by then
//              -> relocate-start pause -> close_selection.
//   acquirer:  store the array in the lowest free slot (seq_cst), or count an
//              overflow when every slot is full -> look the array's page up in
//              the open selections. Before the final scan of that selection it
//              holds the page at once (the final scan sees the slot). After it,
//              a hold already published in a lower slot of the same mutator on
//              that page covers this acquire. The collector reads
//              rawDataOverflow and then the slots in ascending order, so a
//              higher slot does not cover, and an overflow hold (read before
//              every slot) is covered by nothing. Without a cover the acquirer
//              clears the slot, waits in saferegion until that selection closes
//              (after its relocate-start pause), reloads the array from its
//              root and retries.
// The final scan therefore sees this hold or a lower-slot hold of the same
// mutator on the same page, or the acquirer sees the final-scan flag and waits.
// An array is not left held on a page this cycle relocates, and an acquirer
// waits at most for final scan -> drop -> pause.
//
// The final scan runs where no further collector work precedes the pause: old,
// inside the driver lock after the young-root remap (JDK zGeneration.cpp:1050-1063);
// young, after select_relocation_set (install, flip_age_pages with its handshake
// and promotion barrier, forwarding table). A late-dropped old page needs nothing
// more; a late-dropped young page is aged in place and stays young. If its
// forwarding was a promotion, remove_pages also clears the allocation-exit
// guard; that guard is not a store barrier and no promotion barrier runs for a
// page dropped before relocate-start.
// (ZRelocate::flip_age_dropped_pages).
class ZJNICritical {
public:
    static void initialize();

    // Acquire side, called in managed state with the current decoded address of
    // the array. Returns true if the array is now held at `array`. Returns false
    // after having waited for a selection that may move it; the caller reloads the
    // array from its root and calls enter() again.
    static bool enter(Mutator& mutator, uintptr_t array);
    // Release side: drops this mutator's hold of `array` (by address; the array
    // cannot have moved while held). Never dereferences the object.
    static void exit(Mutator& mutator, uintptr_t array);

    // Collector side (ZGeneration::select_relocation_set / collect).
    // Returns the number of selected pages that were deselected for holders.
    static size_t open_selection(ZGenerationId id, ZRelocationSetSelector* selector);
    // Either generation, immediately before the relocate-start pause: final scan.
    // Fills `held` (sorted) with the selected pages that hold an array, or sets
    // `all` if a holder overflowed its slots.
    static void finish_selection(ZGenerationId id, std::vector<ZPage*>& held, bool& all);
    static void close_selection(ZGenerationId id);

    // Diagnostics and unit tests.
    static int64_t count_snapshot();          // mutators currently holding raw data
    static int64_t holds(const Mutator& mutator); // slots + overflow of one mutator
    static bool selection_open(ZGenerationId id);
    static uint64_t closed_selections(ZGenerationId id);
    static uint64_t retreats();               // acquirer waits since start
    static uint64_t deselected_pages();       // selected pages kept for holders, cumulative
    static uint64_t late_dropped_pages();     // pages dropped by the final scans, cumulative
    static void note_late_dropped(size_t n);
    // Unit tests only: publish `pages` as the open selection of `id` without a
    // selector. Returns the number of those pages that scanned holders keep (all
    // of them if a holder overflowed its slots), or SIZE_MAX if `id` already has
    // an open selection.
    static size_t open_selection_for_test(ZGenerationId id, ZPage* const* pages, size_t n);
};

} // namespace MapleRuntime

#endif
