// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator.h"

#include <cinttypes>
#include <cstdint>

#include "Base/ImmortalWrapper.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/MObject.h"

namespace MapleRuntime {
using namespace std;
Allocator::Allocator()
{
    allocBufferManager = new (std::nothrow) AllocBufferManager();
    CHECK_DETAIL(allocBufferManager != nullptr, "new alloc buffer manager failed");
}



// PageAlllocator
// the input parameter cat should be guaranteed in the range of value of enum type AllocationTag by
// external invoker, in order to avoid exceed the border of matrix
AggregateAllocator& AggregateAllocator::Instance(AllocationTag tag)
{
    static ImmortalWrapper<AggregateAllocator> instance[MAX_ALLOCATION_TAG];
    return *(instance[tag]);
}

// PagePool
PagePool& PagePool::Instance() noexcept
{
    static ImmortalWrapper<PagePool> instance("PagePool");
    return *instance;
}

Allocator* Allocator::NewAllocator()
{
    RegionSpace* regionSpace = new (std::nothrow) RegionSpace();
    CHECK_DETAIL(regionSpace != nullptr, "New RegionSpace failed");
    return regionSpace;
}
} // namespace MapleRuntime
