// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_STRING_DEDUP_H
#define MRT_STRING_DEDUP_H

#include <functional>
#include <mutex>
#include <unordered_map>
#include "Common/BaseObject.h"
#include "Common/TypeDef.h"

namespace MapleRuntime {
// Weak table of byte arrays. String is a value type, so this call returns the
// canonical array instead of rewriting a heap String (stringDedupTable.cpp:634).
class StringDedup {
    friend class StringDedupTest;
public:
    static StringDedup& Instance();
    void Start();
    void Stop();
    ArrayRef Canonical(const TypeInfo* arrayInfo, ArrayRef candidate);
    void Clean(const std::function<bool(BaseObject*)>& isAlive);

private:
    struct WeakSlot {
        zpointer value;
    };
    using Table = std::unordered_multimap<size_t, WeakSlot>;
    static bool Accepts(const TypeInfo* arrayInfo, ArrayRef candidate);
    static BaseObject* Resolve(WeakSlot& slot);
    size_t Hash(BaseObject* object) const;
    StringDedup();
    ~StringDedup() { Stop(); }
    std::recursive_mutex mutex;
    bool stopped = true;
    Table table;
    uint64_t hashSeed;
};
} // namespace MapleRuntime
#endif
