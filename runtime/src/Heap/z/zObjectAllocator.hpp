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
        std::atomic<ZPage*> pinnedPage{nullptr};

        ZPage** shared_small_page_addr();
        ZPage* const* shared_small_page_addr() const;
        ZPage** shared_medium_page_addr();
        ZPage* const* shared_medium_page_addr() const;
    };

    ZObjectAllocator();
    PerAge* allocator(PageAge age) { return objectAllocators[untype(age)].get(); }
    uintptr_t alloc(size_t size, PageAge age, bool nonBlocking = false, bool clearPayload = true);
    void retire_pages(PageAgeRange ages);

private:
    ZDeferredConstructed<PerAge> objectAllocators[kPageAgeCount];
};

// zObjectAllocator.cpp:48-54 (per-CPU shared small pages are always in use;
// ZHeuristics::use_per_cpu_shared_small_pages belongs to the allocator package).
inline ZPage** ZObjectAllocator::PerAge::shared_small_page_addr()
{
    return sharedSmallPage.addr();
}

inline ZPage* const* ZObjectAllocator::PerAge::shared_small_page_addr() const
{
    return sharedSmallPage.addr();
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
