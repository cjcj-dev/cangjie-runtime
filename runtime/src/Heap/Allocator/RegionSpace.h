// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_REGION_SPACE_H
#define MRT_REGION_SPACE_H

#include <cassert>
#include <list>
#include <memory>
#include <sys/mman.h>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "Heap/z/zServiceability.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "ExceptionManager.h"
#include "Mutator/Mutator.h"
#include "Heap/z/zPageAllocator.hpp"
namespace MapleRuntime {
}

#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"


#endif

namespace MapleRuntime {
// RegionSpace aims to be the API for other components of runtime
// the complication of implementation is delegated to RegionManager
// allocator should not depend on any assumptions on the details of RegionManager
class RegionSpace {
public:
    static size_t ToAllocSize(size_t objSize)
    {
        size_t size = objSize + HEADER_SIZE;
        return RoundUp<size_t>(size, ALLOC_ALIGN);
    }

    static size_t GetAllocSize(const BaseObject& obj)
    {
        size_t objSize = obj.GetSize();
        return ToAllocSize(objSize);
    }

    static constexpr size_t ALLOC_ALIGN = 8;
    static constexpr size_t HEADER_SIZE = 0;
    RegionSpace() = default;
    Uncommitter& GetUncommitter() { return uncommitter; }
    bool IsHeapAddress(MAddress addr) const { return is_heap_address(addr); }
    ~RegionSpace() = default;

    MAddress Allocate(size_t size, AllocType allocType);

    RegionManager& GetRegionManager() const noexcept;

    size_t AllocatedBytes() const { return GetRegionManager().GetAllocatedSize(); }

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool IsHeapObject(MAddress addr) const;
#endif

    void DumpRegionStats(const char* msg) const
    {
        GetRegionManager().DumpRegionStats(msg);
    }

    void PrepareTrace() { GetRegionManager().PrepareTrace(); }


    // ZPage::mark_object + inc_live (zMark.cpp:405-425) for a caller without a
    // ZMarkCache: the first live claim is accounted on the page directly.
    template<Generation G>
    static bool MarkObject(const BaseObject* obj)
    {
        ZPage* regionInfo = Heap::page(reinterpret_cast<MAddress>(obj));
        (void)G;
        bool incLive = false;
        const bool newlyMarked = regionInfo->mark_object(from_object(obj), false, incLive);
        if (incLive) {
            regionInfo->inc_live(1, obj->GetSize());
        }
        return !newlyMarked;
    }

    // ZPage::is_object_strongly_live (zPage.inline.hpp:258-260).
    template<Generation G>
    static bool IsMarkedObject(const BaseObject* obj)
    {
        (void)G;
        ZPage* regionInfo = Heap::page(reinterpret_cast<MAddress>(obj));
        return regionInfo->is_object_strongly_live(from_object(obj));
    }

    // Finalizable-only marked: the live bit without the strong bit
    // (zPage.inline.hpp:254-260 pair semantics).
    static bool IsResurrectedObject(const BaseObject* obj)
    {
        ZPage* regionInfo = Heap::page(reinterpret_cast<MAddress>(obj));
        const zaddress addr = from_object(obj);
        return regionInfo->is_object_live(addr) && !regionInfo->is_object_strongly_live(addr);
    }


private:
    Uncommitter uncommitter{*this};
    MAddress TryAllocateOnce(size_t allocSize, AllocType allocType);
    MAddress AllocateOutsideTLAB(size_t allocSize, AllocType allocType);

};
} // namespace MapleRuntime
#endif // MRT_REGION_SPACE_H
