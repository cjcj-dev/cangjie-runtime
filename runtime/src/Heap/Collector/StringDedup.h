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
    void Request(BaseObject* object);
    void Clean(const std::function<bool(BaseObject*)>& isAlive);
    void Remap();

    // Corresponds to the processor leaving the suspendible thread set before
    // GC accesses weak storage. The current runtime serializes GC drivers.
    class GCScope {
    public:
        GCScope();
        ~GCScope();
        GCScope(const GCScope&) = delete;
        GCScope& operator=(const GCScope&) = delete;
    };

private:
#if defined(MRT_TESTABLE_INTERNALS)
    friend struct StringDedupTestAccess;
#endif
    struct WeakSlot {
        zpointer value;
    };
    using Table = std::unordered_multimap<size_t, WeakSlot>;
    static bool IsByteArray(BaseObject* object);
    static BaseObject* Resolve(WeakSlot& slot);
    static size_t Hash(BaseObject* object);
    void Run();
    void Process(WeakSlot slot);
    StringDedup() = default;
    ~StringDedup() { Stop(); }
    // Resolution can publish another promotion request on the same GC thread.
    std::recursive_mutex mutex;
    std::condition_variable_any condition;
    std::thread processor;
    bool stopped = true;
    size_t suspended = 0;
    std::vector<WeakSlot> requests;
    Table table;
};
} // namespace MapleRuntime
#endif
