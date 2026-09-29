// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_STRING_DEDUP_H
#define MRT_STRING_DEDUP_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include "Common/WeakHandle.h"
#include "Common/TypeDef.h"

namespace MapleRuntime {
// String is a value type: the synchronous API returns the backing instead of
// updating a heap String.value (stringDedupTable.cpp:603-648).
class StringDedup {
#if defined(MRT_TESTABLE_INTERNALS)
    friend class StringDedupTest;
#endif
    class Table {
#if defined(MRT_TESTABLE_INTERNALS)
        friend class StringDedupTest;
#endif
        class Bucket;
        class CleanupState;
        class Cleaner;
        class Resizer;
        enum class DeadState { good, wait1, wait2, cleaning };
        StringDedup& owner;
        OopStorage storage;
        std::unique_ptr<Bucket[]> buckets;
        size_t numberOfBuckets = 0;
        size_t numberOfEntries = 0;
        size_t growThreshold = 0;
        std::unique_ptr<CleanupState> cleanupState;
        bool needBucketShrinking = false;
        std::atomic<size_t> deadCount{0};
        std::atomic<DeadState> deadState{DeadState::good};
        std::unique_ptr<Bucket[]> MakeBuckets(size_t count, size_t reserve = 0);
        void Add(WeakHandle value, uint32_t hash);
        bool IsDeadCountGoodAcquire() const;
        void SetDeadStateCleaning();
        bool StartResizer(bool growOnly, size_t entries);
        bool StartCleaner();
        static void NumDeadCallback(size_t count);
    public:
        explicit Table(StringDedup& owner);
        ~Table();
        void Initialize();
        void Clear();
        OopStorage& Storage() { return storage; }
        WeakHandle Find(MArray* object, uint32_t hash) const;
        void Install(MArray* object, uint32_t hash);
        bool IsGrowNeeded() const;
        bool IsDeadEntryRemovalNeeded() const;
        bool CleanupStartIfNeeded(bool growOnly, bool force);
        bool CleanupStep();
        void CleanupEnd();
    };
    class Processor;
public:
    static StringDedup& Instance();
    void Start();
    void Stop();
    ArrayRef Canonical(const TypeInfo* arrayInfo, ArrayRef candidate);
    // Stable for the process lifetime, including before the service starts.
    OopStorage& WeakStorage() { return table.Storage(); }
private:
    static bool Accepts(const TypeInfo* arrayInfo, ArrayRef candidate);
    size_t Hash(BaseObject* object) const;
    void NotifyWork();
    StringDedup();
    ~StringDedup();
    // The upstream table has one JavaThread consumer. Synchronous value-type
    // requests require this serial ownership domain for all table operations.
    std::mutex tableMutex;
    std::mutex lifecycleMutex;
    // StringDedup_lock counterpart: never held during a heap access or step.
    std::mutex monitor;
    std::condition_variable condition;
    bool workPending = false;
    bool stopped = true;
    Table table;
    std::unique_ptr<Processor> processor;
    uint64_t hashSeed;
};
} // namespace MapleRuntime
#endif
