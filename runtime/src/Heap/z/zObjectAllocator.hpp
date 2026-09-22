// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_OBJECT_ALLOCATOR_H
#define MRT_OBJECT_ALLOCATOR_H

#include "Heap/z/zDeferredConstructed.inline.hpp"
#include "Heap/z/zValue.inline.hpp"
#include "Heap/z/zLock.hpp"
#include "Heap/z/zPageAge.hpp"
#include "Heap/z/zPageFwd.hpp"
#include <atomic>
#include "Heap/z/zAllocationFlags.hpp"
#include "Heap/z/zPageType.hpp"

namespace MapleRuntime {
class ZObjectAllocator {
public:
    struct PerAge {
        explicit PerAge(PageAge pageAge);
        const PageAge age;
        const bool usePerCpuSharedSmallPages;
        ZPerCPU<ZPage*> sharedSmallPage;
        ZContended<ZPage*> sharedMediumPage;
        ZLock mediumPageAllocLock;

        ZPage* alloc_page(ZPageType type, size_t size, ZAllocationFlags flags, bool clearPayload = true);
        void undo_alloc_page(ZPage* page);
        uintptr_t alloc_object_in_shared_page(ZPage** shared, ZPageType type, size_t pageSize,
                                              size_t size, ZAllocationFlags flags);
        uintptr_t alloc_object_in_medium_page(size_t size, ZAllocationFlags flags);
        uintptr_t alloc_small_object(size_t size, ZAllocationFlags flags);
        uintptr_t alloc_medium_object(size_t size, ZAllocationFlags flags);
        uintptr_t alloc_large_object(size_t size, ZAllocationFlags flags, bool clearPayload);
        uintptr_t alloc_object(size_t size, ZAllocationFlags flags, bool clearPayload);
        void retire_pages();
        ZPage** shared_small_page_addr();
        ZPage* const* shared_small_page_addr() const;
        ZPage** shared_medium_page_addr();
        ZPage* const* shared_medium_page_addr() const;
    };

    ZObjectAllocator();
    PerAge* allocator(PageAge age) { return objectAllocators[untype(age)].get(); }
    const PerAge* allocator(PageAge age) const { return objectAllocators[untype(age)].get(); }
    size_t fast_available(PageAge age) const;
    uintptr_t alloc(size_t size, PageAge age, bool nonBlocking = false, bool clearPayload = true);
    void retire_pages(PageAgeRange ages);

private:
    ZDeferredConstructed<PerAge> objectAllocators[kPageAgeCount];
};

// zObjectAllocator.cpp:48-54 (per-CPU shared small pages are always in use;
// ZHeuristics::use_per_cpu_shared_small_pages belongs to the allocator package).
inline ZPage** ZObjectAllocator::PerAge::shared_small_page_addr()
{
    return usePerCpuSharedSmallPages ? sharedSmallPage.addr() : sharedSmallPage.addr(0);
}

inline ZPage* const* ZObjectAllocator::PerAge::shared_small_page_addr() const
{
    return usePerCpuSharedSmallPages ? sharedSmallPage.addr() : sharedSmallPage.addr(0);
}

inline ZPage** ZObjectAllocator::PerAge::shared_medium_page_addr()
{
    return sharedMediumPage.addr();
}

inline ZPage* const* ZObjectAllocator::PerAge::shared_medium_page_addr() const
{
    return sharedMediumPage.addr();
}

} // namespace MapleRuntime
#endif
