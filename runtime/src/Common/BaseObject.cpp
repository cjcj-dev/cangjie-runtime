// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Common/BaseObject.inline.h"
#include "Common/ColourEncoding.h"
#include "Base/Log.h"
#include "BaseObject.h"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zReferenceProcessor.hpp"
#include "Heap/z/zHeap.hpp"
#include "Mutator/Mutator.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/MObject.h"
#include "ObjectModel/MObject.inline.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
// Keep the out-of-line slot store available to declaration-only consumers.
// Do not depend on a barrier call retaining an incidental template instance:
// OHOS and optimized builds can inline every such call.
// ZGC: zBarrier.inline.hpp:448-450, 692-706 (store-good barrier path).
template void HeapSlot<false>::StoreColoured(zpointer, std::memory_order);

TypeInfo* BaseObject::GetTypeInfo() const { return stateWord.GetTypeInfo(); }

ptrdiff_t BaseObject::referent_offset()
{
    return static_cast<ptrdiff_t>(TYPEINFO_PTR_SIZE);
}

bool BaseObject::is_referent_field(BaseObject* obj, ptrdiff_t offset)
{
    if (offset != referent_offset()) {
        return false;
    }
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return false;
    }
    TypeInfo* klass = obj->GetTypeInfo();
    return klass->IsWeakRefType();
}

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
    heap.MarkNewObject(this);
    // HotSpot sharedRuntime.cpp:1072-1075 / instanceKlass.cpp:1919-1932:
    // constructor completion registers the object before returning to the caller.
    heap.GetFinalizerProcessor().RegisterFinalizer(this);
}

bool BaseObject::IsInTraceRegion() const
{
    ZPage* region = Heap::page(reinterpret_cast<Uptr>(this));
    MRT_ASSERT(region != nullptr, "region is nullptr");
    return region->IsTraceRegion();
}

} // namespace MapleRuntime
