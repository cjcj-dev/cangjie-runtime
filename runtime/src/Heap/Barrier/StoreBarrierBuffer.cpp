// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "StoreBarrierBuffer.h"

#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Allocator/Allocator.h"
#include "Heap/Heap.h"
#include "RememberedSet.h"

namespace MapleRuntime {

void StoreBarrierBuffer::Add(MAddress fieldAddress, RememberedSet& rs)
{
    if (!kBufferStoreBarriers) {
        rs.Record(fieldAddress, true);
        return;
    }
    if (current == 0) {
        Flush(rs);
    }
    --current;
    buffer[current].p = fieldAddress;
}

void StoreBarrierBuffer::Flush(RememberedSet& rs)
{
    if (!kBufferStoreBarriers) {
        return;
    }
    for (size_t i = current; i < kStoreBarrierBufferLength; ++i) {
        rs.Record(buffer[i].p, true);
    }
    current = kStoreBarrierBufferLength;
}

void StoreBarrierBuffer::Discard()
{
    current = kStoreBarrierBufferLength;
}

void StoreBarrierBuffer::FlushAll(RememberedSet& rs)
{
    if (!kBufferStoreBarriers) {
        return;
    }
    Heap::GetHeap().GetAllocator().VisitAllocBuffers([&rs](AllocBuffer& alloc) {
        alloc.GetStoreBarrierBuffer().Flush(rs);
    });
}

} // namespace MapleRuntime
