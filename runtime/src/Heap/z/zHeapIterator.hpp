// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_HEAP_ITERATOR_H
#define MRT_HEAP_ITERATOR_H

#include <functional>
#include <unordered_set>
#include <vector>
#include "Common/BaseObject.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
// zHeapIterator.cpp:195-229,517-545. One root-seeded graph for verification and inspection.
class HeapIterator {
public:
    using ObjectVisitor = std::function<void(BaseObject*)>;
    using FieldVisitor = std::function<void(BaseObject*, RefField<>&)>;
    using EdgeVisitor = std::function<void(BaseObject*, const void*, uintptr_t)>;
    explicit HeapIterator(bool visitWeaks, bool forVerify = false) : visitWeaks(visitWeaks), forVerify(forVerify) {}
    void Iterate(const ObjectVisitor& objectVisitor, const EdgeVisitor& fieldVisitor = {});
    static void Fields(BaseObject* object, bool visitReferents, const FieldVisitor& visitor);
private:
    void Push(BaseObject* object, const ObjectVisitor& objectVisitor);
    const bool visitWeaks;
    const bool forVerify;
    std::unordered_set<BaseObject*> visited;
    std::vector<BaseObject*> stack;
};
} // namespace MapleRuntime
#endif
