// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Standalone model check for C3 epoch/store-good state machine.
// Build (header-only, no runtime link):
//   clang++ -std=gnu++14 -O0 -I runtime/src runtime/tests/c3epoch_state_machine_probe.cpp -o c3epoch-probe

#include <cstdio>
#include <cstdlib>

#include "Common/C3EpochStateMachine.h"

using namespace MapleRuntime;
using namespace MapleRuntime::C3;

static int failures = 0;

static void expect(bool cond, const char* msg)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    } else {
        std::printf("PASS: %s\n", msg);
    }
}

static void print_stats(const char* label, const ModelStats& s)
{
    std::printf("%s cells=%llu non_null=%llu uncertain=%llu trust_like=%llu "
                "load_good=%llu mark_good=%llu store_good=%llu\n",
                label, static_cast<unsigned long long>(s.cells),
                static_cast<unsigned long long>(s.nonNullCells),
                static_cast<unsigned long long>(s.uncertain),
                static_cast<unsigned long long>(s.trustLike),
                static_cast<unsigned long long>(s.loadGood),
                static_cast<unsigned long long>(s.markGood),
                static_cast<unsigned long long>(s.storeGood));
}

static ModelStats run_flip_sequence()
{
    ModelStats total {};
    EpochView e {};
    auto add = [&](const EpochView& view) {
        ModelStats part = enumerate_epoch(view, DropBit::None);
        total.cells += part.cells;
        total.nonNullCells += part.nonNullCells;
        total.uncertain += part.uncertain;
        total.trustLike += part.trustLike;
        total.loadGood += part.loadGood;
        total.markGood += part.markGood;
        total.storeGood += part.storeGood;
    };

    add(e);
    flip_young_mark_start(e);
    add(e);
    flip_young_relocate_start(e);
    add(e);
    flip_old_mark_start(e);
    add(e);
    flip_old_relocate_start(e);
    add(e);
    // Interleaved: young mark while old relocating residue
    flip_young_mark_start(e);
    add(e);
    return total;
}

static void check_store_good_after_young_mark()
{
    EpochView e {};
    PointerColour p {};
    p.remapCode = 0; // Remapped00 == current
    p.markedYoung = 0;
    p.markedOld = 0;
    p.remembered = 0;
    p.finalizable = 0;
    BarrierJudgement before = judge(p, e);
    expect(before.loadGood && before.markGood && before.storeGood, "initial colour is store-good");

    flip_young_mark_start(e);
    BarrierJudgement after = judge(p, e);
    expect(after.loadGood, "young mark-start keeps load-good (remap untouched)");
    expect(!after.markGood, "young mark-start makes previous MY mark-bad");
    expect(!after.storeGood, "young mark-start makes previous Rem store-bad");
}

static void check_double_remap_selection()
{
    EpochView e {};
    // Force double remap-bad: flip both relocate epochs away from Remapped00.
    flip_young_relocate_start(e);
    flip_old_relocate_start(e);
    PointerColour p {};
    p.remapCode = 0;
    p.markedYoung = e.markedYoung;
    p.markedOld = e.markedOld;
    p.remembered = 2; // both bits
    p.finalizable = e.finalizable;

    BarrierJudgement j = judge(p, e);
    expect(!j.loadGood, "double-remap-bad is load-bad");

    ForwardingPresence empty {};
    expect(select_remap_generation(p, e, empty) == RemapGeneration::Old,
           "double rem bits select old table");

    p.remembered = 0;
    ForwardingPresence youngOnly {};
    youngOnly.inYoungTable = true;
    expect(select_remap_generation(p, e, youngOnly) == RemapGeneration::Young,
           "single rem + young table selects young");

    ForwardingPresence both {};
    both.inYoungTable = true;
    both.inOldTable = true;
    expect(select_remap_generation(p, e, both) == RemapGeneration::Uncertain,
           "mutual exclusion violation is uncertain");
}

static void check_plain_never_good()
{
    EpochView e {};
    PointerColour p {};
    p.plainNonNull = true;
    BarrierJudgement j = judge(p, e);
    expect(!j.loadGood && !j.markGood && !j.storeGood, "plain non-null is never good");
}

static uint64_t uncertain_with_double_bad(DropBit d)
{
    EpochView e {};
    uint64_t u = enumerate_epoch(e, d).uncertain;
    flip_young_relocate_start(e);
    flip_old_relocate_start(e);
    u += enumerate_epoch(e, d).uncertain;
    return u;
}

static void check_drop_bits()
{
    expect(uncertain_with_double_bad(DropBit::None) == 0, "full model uncertain==0");
    expect(uncertain_with_double_bad(DropBit::Remembered) > 0, "DROP remembered => uncertain>0");
    expect(uncertain_with_double_bad(DropBit::Finalizable) > 0, "DROP finalizable => uncertain>0");
    expect(uncertain_with_double_bad(DropBit::MarkedYoung) > 0, "DROP marked_young => uncertain>0");
    expect(uncertain_with_double_bad(DropBit::MarkedOld) > 0, "DROP marked_old => uncertain>0");
    expect(uncertain_with_double_bad(DropBit::RemapSplit) > 0, "DROP remap_split => uncertain>0");

    std::printf("DROP_BIT_none uncertain=%llu\n",
                static_cast<unsigned long long>(uncertain_with_double_bad(DropBit::None)));
    std::printf("DROP_BIT_remembered uncertain=%llu\n",
                static_cast<unsigned long long>(uncertain_with_double_bad(DropBit::Remembered)));
    std::printf("DROP_BIT_finalizable uncertain=%llu\n",
                static_cast<unsigned long long>(uncertain_with_double_bad(DropBit::Finalizable)));
    std::printf("DROP_BIT_marked_young uncertain=%llu\n",
                static_cast<unsigned long long>(uncertain_with_double_bad(DropBit::MarkedYoung)));
    std::printf("DROP_BIT_marked_old uncertain=%llu\n",
                static_cast<unsigned long long>(uncertain_with_double_bad(DropBit::MarkedOld)));
    std::printf("DROP_BIT_remap_split uncertain=%llu\n",
                static_cast<unsigned long long>(uncertain_with_double_bad(DropBit::RemapSplit)));
}

static void check_padding_budget()
{
    expect(REMEMBERED_BITS + FINALIZABLE_BITS <= TAG_ID_PADDING_BITS,
           "padding budget holds for Rem+Fin");
    expect(C3_SPARE_PADDING_BITS + REMEMBERED_BITS + FINALIZABLE_BITS == TAG_ID_PADDING_BITS ||
               TAG_ID_PADDING_BITS < REMEMBERED_BITS + FINALIZABLE_BITS,
           "spare + rem + fin accounts for padding (or static_assert already fired)");
    std::printf("PADDING rem_shift=%u fin_shift=%u spare=%u tag_pad=%u\n", REMEMBERED_SHIFT,
                FINALIZABLE_SHIFT, C3_SPARE_PADDING_BITS, TAG_ID_PADDING_BITS);
}

int main()
{
    check_padding_budget();
    check_plain_never_good();
    check_store_good_after_young_mark();
    check_double_remap_selection();

    ModelStats full = run_flip_sequence();
    print_stats("FULL_FLIP_SEQ", full);
    expect(full.uncertain == 0, "full flip sequence uncertain==0");
    expect(full.trustLike == 0, "full flip sequence trust_like==0");
    expect(full.storeGood > 0 && full.storeGood <= full.markGood && full.markGood <= full.loadGood,
           "store ⊆ mark ⊆ load good counts");

    check_drop_bits();

    if (failures != 0) {
        std::fprintf(stderr, "C3EPOCH_PROBE failures=%d\n", failures);
        return 1;
    }
    std::printf("C3EPOCH_PROBE OK failures=0\n");
    return 0;
}
