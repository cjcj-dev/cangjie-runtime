// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cstdlib>

#include "Common/ColourMask.h"
#include "Base/Log.h"
#include "BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/MObject.h"
#include "ObjectModel/MObject.inline.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
// COLOUR_WRITEBACK_AUDIT §六 判据 1：唯一落笔 choke（单一定义，默认关）。
void AssertColouredWriteIfEnabled(const void* slot, MAddress newVal)
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_ASSERT_COLOURED_WRITES");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    if (LIKELY(!on)) {
        return;
    }
    if (!Heap::IsHeapAddress(slot)) {
        return;
    }
    if ((newVal & ((MAddress(1) << 48) - 1)) == 0) {
        return;
    }
    constexpr MAddress kColourMask = REMAP_COLOUR_MASK | MARKED_YOUNG_MASK | MARKED_OLD_MASK;
    bool hasColour = (newVal & kColourMask) != 0;
    bool tagged = ((newVal >> 48) & 1) != 0;
    bool loadGood = (newVal & static_cast<MAddress>(::g_cjLoadBadMask)) == 0;
    CHECK_DETAIL(hasColour && (loadGood || tagged),
                 "MRT_GCV2_ASSERT_COLOURED_WRITES: plain/bad-colour heap ref write @%p val=%#zx "
                 "hasColour=%d loadGood=%d tagged=%d",
                 slot, newVal, hasColour, loadGood, tagged);
}

TypeInfo* BaseObject::GetTypeInfo() const { return stateWord.GetTypeInfo(); }

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
void BaseObject::DumpObject(int logtype, bool isSimple) const
{
    static constexpr size_t DUMP_WORDS_PER_LINE = 4;
    size_t objSize = GetSize();

    DLOG(LogType(logtype), "[obj] %p %p %zu", this, GetTypeInfo(), objSize);
    if (isSimple) {
        return;
    }
    // dump more details
    size_t word = RoundUp(objSize, sizeof(void*)) / sizeof(void*);
    MAddress obj = reinterpret_cast<MAddress>(this);
    constexpr size_t bufferSize = 256;
    char buf[bufferSize];
    for (size_t i = 0; i < word; i += DUMP_WORDS_PER_LINE) {
        int index = sprintf_s(buf, sizeof(buf), "%zx: ", (obj + (i * sizeof(MAddress))));
        size_t bound = (i + DUMP_WORDS_PER_LINE) < word ? (i + DUMP_WORDS_PER_LINE) : word;
        for (size_t j = i; j < bound; j++) {
            index += sprintf_s(buf + index, sizeof(buf) - static_cast<size_t>(index), "%p ",
                               *reinterpret_cast<ObjectPtr*>(obj + (j * sizeof(MAddress))));
#ifdef USE_32BIT_REF
            index += sprintf_s(buf + index, sizeof(buf) - static_cast<size_t>(index), "%p ",
                               *reinterpret_cast<ObjectPtr*>(obj + (j * sizeof(MAddress) + sizeof(ObjectPtr))));
#endif // USE_32BIT_REF
        }
        DLOG(LogType(logtype), buf);
    }
}
#endif

static void ForEachRefFieldInNonArrayObject(ObjectPtr obj, const RefFieldVisitor& visitor)
{
    GCTib gcTib = obj->GetGCTib();
    // gcTib record payload data, skip the TypeInfo
    MAddress objAddr = reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    gcTib.ForEachBitmapWord(objAddr, visitor);
}

// Call func on each element in an object array.
static void ForEachElementInArray(ObjectPtr obj, const RefFieldVisitor& visitor)
{
    // take array length and content.
    MArray* mArray = reinterpret_cast<MArray*>(obj);
    MIndex arrayLengthVal = mArray->GetLength();
    TypeInfo* componentTypeInfo = mArray->GetComponentTypeInfo();
    if (componentTypeInfo->IsStructType()) {
        GCTib gcTib = componentTypeInfo->GetGCTib();
        MAddress contentAddr = reinterpret_cast<Uptr>(mArray) + MArray::GetContentOffset();
        for (MIndex i = 0; i < arrayLengthVal; ++i) {
            gcTib.ForEachBitmapWord(contentAddr, visitor);
            contentAddr += mArray->GetElementSize();
        }
    } else if (componentTypeInfo->IsObjectType() || componentTypeInfo->IsArrayType() ||
               componentTypeInfo->IsInterface()) {
        RefField<>* arrayContent = reinterpret_cast<RefField<>*>(mArray->ConvertToCArray());
        // for each object in array.
        for (MIndex i = 0; i < arrayLengthVal; ++i) {
            visitor(arrayContent[i]);
        }
    } else {
        LOG(RTLOG_FATAL, "array object %p has wrong component type", mArray);
    }
}

void BaseObject::ForEachRefField(const RefFieldVisitor& visitor)
{
    TypeInfo* typeInfo = GetTypeInfo();
    if (typeInfo->HasRefField()) {
        if (UNLIKELY(typeInfo->IsRawArray())) {
            ForEachElementInArray(this, visitor);
        } else {
            ForEachRefFieldInNonArrayObject(this, visitor);
        }
    }
};

void BaseObject::ForEachRefInStruct(const RefFieldVisitor& visitor, MAddress aggStart, MAddress aggEnd)
{
    TypeInfo* typeInfo = GetTypeInfo();
    if (typeInfo->HasRefField()) {
        if (UNLIKELY(typeInfo->IsRawArray())) {
            ForEachAggRefFieldInArray(visitor, aggStart, aggEnd);
        } else {
            ForEachAggRefFieldInNonArray(visitor, aggStart, aggEnd);
        }
    }
}

void BaseObject::ForEachAggRefFieldInArray(const RefFieldVisitor& visitor, MAddress aggStart, MAddress aggEnd)
{
    // take array length and content.
    MArray* mArray = static_cast<MArray*>(this);
    MIndex arrayLen = mArray->GetLength();
    TypeInfo* component = mArray->GetComponentTypeInfo();
    if (component->IsStructType()) {
        GCTib gcTib = component->GetGCTib();
        MAddress contentAddr = reinterpret_cast<Uptr>(this) + MArray::GetContentOffset();
        size_t contentSize = mArray->GetElementSize();
        // MIndex is enough to describe the size;
        MIndex startIndex = static_cast<MIndex>((aggStart - contentAddr) / contentSize);
        size_t alignedStart = startIndex * contentSize + contentAddr;
        MRT_ASSERT((alignedStart + contentSize) >= aggEnd, "aggregate element is not align\n");
        MAddress currentAddr = alignedStart;
        for (U64 i = startIndex; (i < arrayLen) && (currentAddr < aggEnd); ++i) {
            gcTib.ForEachBitmapWordInRange(currentAddr, visitor, aggStart, aggEnd);
            currentAddr += contentSize;
        }
    } else {
        LOG(RTLOG_FATAL, "this interface mustn't be invoked by array whose element is not record");
    }
}

void BaseObject::ForEachAggRefFieldInNonArray(const RefFieldVisitor& visitor, MAddress aggStart, MAddress aggEnd) const
{
    // gcTib record payload data, skip the TypeInfo
    GetGCTib().ForEachBitmapWordInRange(reinterpret_cast<MAddress>(this) + TYPEINFO_PTR_SIZE, visitor, aggStart,
                                        aggEnd);
}

size_t BaseObject::GetSize() const
{
    TypeInfo* kls = GetTypeInfo();
    if (kls->IsArrayType()) {
        const MArray* mArray = reinterpret_cast<const MArray*>(this);
        size_t size = mArray->GetMArraySize();
        return MapleRuntime::AlignUp<size_t>(size, AllocatorUtils::ALLOC_ALIGNMENT);
    } else {
        return MapleRuntime::AlignUp<size_t>(kls->GetInstanceSize() + TYPEINFO_PTR_SIZE,
                                             AllocatorUtils::ALLOC_ALIGNMENT);
    }
}

void BaseObject::OnFinalizerCreated()
{
    Heap& heap = Heap::GetHeap();
    heap.GetCollector().MarkNewObject(this);
    Mutator* mutator = Mutator::GetMutator();
    if (mutator != nullptr) {
        mutator->AddLocalFinalizer(this);
    } else {
        heap.GetFinalizerProcessor().RegisterFinalizer(this);
    }
}

bool BaseObject::IsInTraceRegion() const
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<Uptr>(this));
    MRT_ASSERT(region != nullptr, "region is nullptr");
    return region->IsTraceRegion();
}

bool BaseObject::CompareExchangeRefField(RefField<>& field, const RefField<> oldRef, const RefField<> newRef)
{
    if (field.CompareExchange(oldRef.GetFieldValue(), newRef.GetFieldValue())) {
        DLOG(BARRIER, "update obj %p ref-field@%p: %#zx => %#zx", raw(oldRef.GetFieldValue()), raw(newRef.GetFieldValue()));
        return true;
    }
    return false;
}
} // namespace MapleRuntime

// Phase B: the read-barrier mask the compiler tests against (see TypeDef.h for why).
// "All colour bits set" keeps the predicate identical to the shift form it replaces.
// Phase C: bad = mid-evacuation, or carrying a colour other than the one being handed out.
// The initial conceptual state is RemappedYoung0 x RemappedOld0, encoded by Remapped00.
extern "C" unsigned long g_cjLoadBadMask =
    MapleRuntime::TAGGED_BITS_MASK |
    (MapleRuntime::REMAP_COLOUR_MASK ^ MapleRuntime::ZPointerRemapped00);

// Mark-good includes load-good plus the current young and old mark epochs. The initial current
// epochs are *_0, so their *_1 bits are bad (OpenJDK zAddress.cpp:78-87,120-127).
extern "C" MRT_EXPORT unsigned long g_cjMarkBadMask = MapleRuntime::TAGGED_BITS_MASK |
    (MapleRuntime::REMAP_COLOUR_MASK ^ MapleRuntime::ZPointerRemapped00) |
    MapleRuntime::MARKED_YOUNG_1 | MapleRuntime::MARKED_OLD_1;
