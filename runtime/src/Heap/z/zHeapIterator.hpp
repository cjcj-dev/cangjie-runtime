// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_HEAP_ITERATOR_H
#define MRT_HEAP_ITERATOR_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include "Common/BaseObject.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
class MArray;
class HeapIteratorBitMap {
    std::vector<std::atomic<uintptr_t>> words;
public:
    explicit HeapIteratorBitMap(size_t bits)
        : words((bits + (sizeof(uintptr_t) * 8 - 1)) / (sizeof(uintptr_t) * 8))
    {
        for (auto& word : words) {
            word.store(0, std::memory_order_relaxed);
        }
    }
    bool try_set_bit(size_t index)
    {
        const size_t bits = sizeof(uintptr_t) * 8;
        auto& word = words[index / bits];
        const uintptr_t mask = uintptr_t(1) << (index % bits);
        uintptr_t old = word.load(std::memory_order_relaxed);
        while ((old & mask) == 0) {
            if (word.compare_exchange_weak(old, old | mask, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }
};

class HeapIterator {
public:
    using ObjectVisitor = std::function<void(BaseObject*)>;
    using FieldVisitor = std::function<void(BaseObject*, RefField<>&)>;
    using EdgeVisitor = std::function<void(BaseObject*, const void*, uintptr_t)>;
    explicit HeapIterator(bool visitWeaks, bool forVerify = false);
    void Iterate(const ObjectVisitor& objectVisitor, const EdgeVisitor& fieldVisitor = {});
    static void Fields(BaseObject* object, bool visitReferents, const FieldVisitor& visitor);
private:
    struct ObjArrayTask {
        MArray* object;
        MIndex index;
    };
    void Push(BaseObject* object, const ObjectVisitor& objectVisitor);
    void Follow(BaseObject* object, const FieldVisitor& visitor);
    void FollowArray(MArray* object);
    void FollowArrayChunk(const ObjArrayTask& array, const FieldVisitor& visitor);
    bool try_set_bit(BaseObject* object);
    const bool visitWeaks;
    const bool forVerify;
    std::unordered_map<uintptr_t, std::unique_ptr<HeapIteratorBitMap>> objectBitmaps;
    std::vector<BaseObject*> stack;
    std::vector<ObjArrayTask> arrayStack;
};
} // namespace MapleRuntime
#endif
