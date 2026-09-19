// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_STRING_DEDUP_H
#define MRT_STRING_DEDUP_H

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "Common/BaseObject.h"

namespace MapleRuntime {
// L01s: weak runtime slots hold byte arrays, never language String values.
// Neither requests nor table entries are enumerated as strong roots.
class StringDedup {
public:
    static StringDedup& Instance();
    void Start();
    void Stop();
    // Only the explicit String ABI supplies immutable content, pinned for this call.
    // RawArray type alone is insufficient: ordinary byte arrays are mutable.
    void RequestString(const uint8_t* data, size_t length);
    void Clean(const std::function<bool(BaseObject*)>& isAlive);

private:
    struct WeakSlot {
        zpointer value;
    };
    using Table = std::unordered_multimap<size_t, WeakSlot>;
    static BaseObject* Resolve(WeakSlot& slot);
    size_t Hash(BaseObject* object) const;
    void Run();
    void Process(WeakSlot slot);
    StringDedup();
    ~StringDedup() { Stop(); }
    // Resolution can publish another promotion request on the same GC thread.
    std::recursive_mutex mutex;
    std::condition_variable_any condition;
    std::thread processor;
    bool stopped = true;
    size_t suspended = 0;
    std::vector<WeakSlot> requests;
    std::vector<WeakSlot> processing;
    Table table;
    uint64_t hashSeed;
};
} // namespace MapleRuntime
#endif
