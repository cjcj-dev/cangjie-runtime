// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_WEAK_REF_BUFFER_H
#define MRT_WEAK_REF_BUFFER_H
#include "Heap/Heap.h"
#include "Heap/Barrier/Barrier.h"
#include "ObjectModel/RefField.inline.h"
#include <mutex>
#include <unordered_set>
namespace MapleRuntime {
class WeakRefBuffer {
public:
    static WeakRefBuffer& Instance() noexcept;
    void ClearWeakRefBuffer()
    {
        for (BaseObject* obj : refObjBuffer) {
            HeapSlot<>& referentField = HeapSlotAt<>(reinterpret_cast<uintptr_t>(obj) + TYPEINFO_PTR_SIZE);
            Heap::GetBarrier().ReadWeakRef(obj, referentField);
        }
        refObjBuffer.clear();
    }
    // insert weakref obj into buffer
    void Insert(BaseObject* obj)
    {
        std::lock_guard<std::mutex> lock(mtx); // For potential concurrency problems
        refObjBuffer.insert(obj);
    }
private:
    std::unordered_set<BaseObject*> refObjBuffer;
    std::mutex mtx;
};
} // namespace MapleRuntime

#endif // MRT_WEAK_REF_BUFFER_H
