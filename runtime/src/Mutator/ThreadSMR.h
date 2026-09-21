// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace MapleRuntime {
class Mutator;
class SafeThreadsListPtr;
struct SMRThread;

// HotSpot runtime/threadSMR.hpp:163: immutable membership, plus the retired
// list link and the reference count used by nested handles.
class ThreadsList {
    friend class ThreadsSMRSupport;
    friend class SafeThreadsListPtr;
    std::vector<Mutator*> threads;
    ThreadsList* next = nullptr;
    std::atomic<size_t> nestedHandleCount{0};
public:
    size_t length() const { return threads.size(); }
    Mutator* thread_at(size_t index) const { return threads[index]; }
    bool includes(const Mutator* thread) const;
};

class ThreadsSMRSupport {
    friend class SafeThreadsListPtr;
    friend struct SMRThread;
    static void free_list(ThreadsList* list);
    static void release_stable_list_wake_up();
public:
    static ThreadsList* get_java_thread_list();
    static void add_thread(Mutator* thread);
    static void remove_thread(Mutator* thread);
    static bool is_a_protected_JavaThread(Mutator* thread);
    static void wait_until_not_protected(Mutator* thread);
    static void smr_delete(Mutator* thread);
};

// threadSMR.hpp:237: leaf hazard pointer; promote the previous handle to a
// list reference count on nested acquisition. Handles stay on their creator.
class SafeThreadsListPtr {
    SafeThreadsListPtr* previous = nullptr;
    SMRThread* thread;
    ThreadsList* heldList = nullptr;
    bool hasRefCount = false;
    void acquire_stable_list();
    void acquire_stable_list_fast_path();
    void acquire_stable_list_nested_path();
    void release_stable_list();
public:
    SafeThreadsListPtr();
    ~SafeThreadsListPtr();
    SafeThreadsListPtr(const SafeThreadsListPtr&) = delete;
    SafeThreadsListPtr& operator=(const SafeThreadsListPtr&) = delete;
    ThreadsList* list() const { return heldList; }
};

class ThreadsListHandle {
    SafeThreadsListPtr listPtr;
public:
    ThreadsList* list() const { return listPtr.list(); }
    size_t length() const { return list()->length(); }
    Mutator* thread_at(size_t index) const { return list()->thread_at(index); }
    bool includes(const Mutator* thread) const { return list()->includes(thread); }
};
} // namespace MapleRuntime
