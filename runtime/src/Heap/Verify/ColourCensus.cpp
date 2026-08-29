// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Verify/ColourCensus.h"

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Common/ColourEncoding.h"
#include "Heap/Heap.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace {
void EnforceColourCensus(const ColourCensusStats& stats, bool armed)
{
    CHECK_DETAIL(!armed || stats.illegal == 0,
                 "armed pointer-colour census found illegal heap slot: slot=%p value=%#zx holder=%p illegal=%zu",
                 stats.firstIllegalSlot, static_cast<size_t>(stats.firstIllegalValue),
                 stats.firstIllegalHolder, stats.illegal);
}
} // namespace

void ColourCensusStats::Observe(const void* slot, uintptr_t value, BaseObject* holder)
{
    ++total;
    switch (ClassifySlotWord(value)) {
        case SlotWordVerdict::kNull:
            ++nulls;
            break;
        case SlotWordVerdict::kLegacyPlain:
            ++legacyPlain;
            if (firstPlainSlot == nullptr) {
                firstPlainSlot = slot;
                firstPlainValue = value;
                firstPlainHolder = holder;
            }
            break;
        case SlotWordVerdict::kColoured:
            ++coloured;
            break;
        case SlotWordVerdict::kIllegal:
            ++illegal;
            if (firstIllegalSlot == nullptr) {
                firstIllegalSlot = slot;
                firstIllegalValue = value;
                firstIllegalHolder = holder;
            }
            break;
    }
}

void CensusObjectSlots(BaseObject* object, ColourCensusStats& stats)
{
    if (object == nullptr || !object->HasRefField()) {
        return;
    }
    object->ForEachRefField([object, &stats](HeapSlot<>& field) {
        stats.Observe(&field, raw(field.GetFieldValue()), object);
    });
}

void VerifyColourCensus(const char* point)
{
    ColourCensusStats stats;
    Heap::GetHeap().ForEachObj([&stats](BaseObject* object) { CensusObjectSlots(object, stats); }, false);
    VLOG(REPORT, "[ptrcolour][census] total=%zu null=%zu coloured=%zu plain=%zu illegal=%zu",
         stats.total, stats.nulls, stats.coloured, stats.legacyPlain, stats.illegal);
    if (stats.legacyPlain != 0) {
        VLOG(REPORT,
             "[ptrcolour][census][plain-first] point=%s slot=%p value=%#zx holder=%p count=%zu",
             point == nullptr ? "?" : point, stats.firstPlainSlot,
             static_cast<size_t>(stats.firstPlainValue), stats.firstPlainHolder, stats.legacyPlain);
    }
    if (stats.illegal != 0) {
        VLOG(REPORT,
             "[ptrcolour][census][illegal-first] point=%s slot=%p value=%#zx holder=%p count=%zu",
             point == nullptr ? "?" : point, stats.firstIllegalSlot,
             static_cast<size_t>(stats.firstIllegalValue), stats.firstIllegalHolder, stats.illegal);
    }
    EnforceColourCensus(stats, ColouredWritesArmed());
}

#if defined(MRT_TESTABLE_INTERNALS)
void EnforceColourCensusForTesting(const ColourCensusStats& stats, bool armed)
{
    EnforceColourCensus(stats, armed);
}
#endif

} // namespace MapleRuntime
