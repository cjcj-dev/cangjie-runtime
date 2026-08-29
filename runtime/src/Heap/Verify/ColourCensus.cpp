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
const char* HolderTypeName(BaseObject* holder)
{
    if (holder == nullptr) {
        return "?";
    }
    TypeInfo* typeInfo = holder->GetTypeInfo();
    return typeInfo == nullptr || typeInfo->GetName() == nullptr ? "?" : typeInfo->GetName();
}

void EnforceColourCensus(const ColourCensusStats& stats)
{
    CHECK_DETAIL(stats.coloured == stats.total && stats.plain == 0 && stats.illegal == 0,
                 "full-colour census rejected heap slot: slots=%zu total=%zu coloured=%zu "
                 "plain_slot=%p plain_value=%#zx plain_holder=%p plain_holder_type=%s plain=%zu "
                 "illegal_slot=%p illegal_value=%#zx illegal_holder=%p illegal_holder_type=%s illegal=%zu",
                 stats.slots, stats.total, stats.coloured,
                 stats.firstPlainSlot, static_cast<size_t>(stats.firstPlainValue),
                 stats.firstPlainHolder, HolderTypeName(stats.firstPlainHolder), stats.plain,
                 stats.firstIllegalSlot, static_cast<size_t>(stats.firstIllegalValue),
                 stats.firstIllegalHolder, HolderTypeName(stats.firstIllegalHolder), stats.illegal);
}
} // namespace

void ColourCensusStats::Observe(const void* slot, uintptr_t value, BaseObject* holder)
{
    ++slots;
    if (value != 0) {
        ++total;
    }
    if (IsPlainNonNullSlotWord(value)) {
        ++plain;
        if (firstPlainSlot == nullptr) {
            firstPlainSlot = slot;
            firstPlainValue = value;
            firstPlainHolder = holder;
        }
        return;
    }
    switch (ClassifySlotWord(value)) {
        case SlotWordVerdict::kNull:
            ++nulls;
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
    VLOG(REPORT, "[ptrcolour][census] slots=%zu total=%zu null=%zu coloured=%zu plain=%zu illegal=%zu",
         stats.slots, stats.total, stats.nulls, stats.coloured, stats.plain, stats.illegal);
    if (stats.plain != 0) {
        VLOG(REPORT,
             "[ptrcolour][census][plain-first] point=%s slot=%p value=%#zx holder=%p count=%zu",
             point == nullptr ? "?" : point, stats.firstPlainSlot,
             static_cast<size_t>(stats.firstPlainValue), stats.firstPlainHolder, stats.plain);
    }
    if (stats.illegal != 0) {
        VLOG(REPORT,
             "[ptrcolour][census][illegal-first] point=%s slot=%p value=%#zx holder=%p count=%zu",
             point == nullptr ? "?" : point, stats.firstIllegalSlot,
             static_cast<size_t>(stats.firstIllegalValue), stats.firstIllegalHolder, stats.illegal);
    }
    EnforceColourCensus(stats);
}

#if defined(MRT_TESTABLE_INTERNALS)
void EnforceColourCensusForTesting(const ColourCensusStats& stats)
{
    EnforceColourCensus(stats);
}
#endif

} // namespace MapleRuntime
