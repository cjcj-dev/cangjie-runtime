// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "SatbBuffer.h"
#include "Heap/Allocator/RegionSpace.h"

#include "Base/ImmortalWrapper.h"

namespace MapleRuntime {
static ImmortalWrapper<SatbBuffer> g_instance;

SatbBuffer& SatbBuffer::Instance() noexcept { return *g_instance; }

bool SatbBuffer::ShouldEnqueue(const BaseObject* obj)
{
    if (UNLIKELY(obj == nullptr)) {
        return false;
    }
    return RegionSpace::ShouldEnqueue(obj);
}

void SatbBuffer::Filter(Node* node)
{
    size_t retainedIndex = Node::CONTAINER_CAPACITY;
    size_t sourceIndex = Node::CONTAINER_CAPACITY;
    while (sourceIndex != node->index) {
        BaseObject* obj = node->objectContainer[--sourceIndex];
        if (Heap::IsHeapAddress(obj) && ShouldEnqueue(obj)) {
            node->objectContainer[--retainedIndex] = obj;
        }
    }
    while (node->index != retainedIndex) {
        node->objectContainer[node->index++] = nullptr;
    }
}

void SatbBuffer::FlushQueue(Node*& node)
{
    if (node == nullptr) {
        return;
    }
    Filter(node);
    if (node->IsEmpty()) {
        freeNodes.Push(node);
    } else {
        retiredNodes.Push(node);
    }
    node = nullptr;
}

static ImmortalWrapper<WeakRefBuffer> g_weakRefBuffer;

WeakRefBuffer& WeakRefBuffer::Instance() noexcept { return *g_weakRefBuffer; }
} // namespace MapleRuntime
