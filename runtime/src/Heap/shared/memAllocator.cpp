// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/shared/memAllocator.hpp"
#include "HeapManager.h"
#include "ObjectModel/MArray.inline.h"
#include <cstring>
namespace MapleRuntime {
MArray* MemAllocator::allocate() const
{
    const MAddress address = HeapManager::Allocate(arraySize, AllocType::MOVEABLE_OBJECT);
    return address == NULL_ADDRESS ? nullptr : initialize(address);
}
void MemAllocator::mem_clear(MAddress address) const
{
    // MArray::NewKnownWidthArray validates the size before allocate() obtains
    // the whole word-aligned extent. As in HotSpot memAllocator.cpp:366-374,
    // clear that owned payload, including tail padding, before publishing klass.
    DCHECK_D(address != NULL_ADDRESS, "cannot initialize null object");
    DCHECK_D(arraySize >= sizeof(BaseObject), "unexpected object size");
    const size_t bytes = AlignUp(arraySize, sizeof(uintptr_t)) - sizeof(BaseObject);
    std::memset(reinterpret_cast<void*>(address + sizeof(BaseObject)), 0, bytes);
}
MArray* MemAllocator::finish(MAddress address) const
{
    return reinterpret_cast<MArray*>(BaseObject::SetClassInfo(address, &arrayClass));
}
MArray* ObjArrayAllocator::initialize(MAddress address) const
{
    // HotSpot memAllocator.cpp:399-410: payload and length precede klass.
    if (doZero) {
        mem_clear(address);
    }
    reinterpret_cast<MArray*>(address)->SetLength(nElems);
    return finish(address);
}
}
