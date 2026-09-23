// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "ThreadSMR.h"
#include "Mutator.h"
#include <algorithm>
#include <condition_variable>
#include <list>
#include <mutex>
#include <vector>

namespace MapleRuntime {
// These are executor fields (HotSpot Thread), not logical Mutator fields
// (JavaThread). GC/native executors can own handles without a Mutator.
struct SMRThread {
    std::atomic<uintptr_t> hazard{0};
    SafeThreadsListPtr* listPtr = nullptr;
    SMRThread();
    ~SMRThread();
};
namespace {
struct SMRState {
    std::mutex mutex;
    std::condition_variable released;
    ThreadsList bootstrap;
    std::atomic<ThreadsList*> current{&bootstrap};
    ThreadsList* retired = nullptr;
    std::list<SMRThread*> executors;
};
SMRState& State()
{
    static SMRState state;
    return state;
}
SMRThread& CurrentExecutor()
{
    thread_local SMRThread thread;
    return thread;
}
constexpr uintptr_t TAG = 1;
#if defined(MRT_TESTABLE_INTERNALS)
std::atomic<void (*)()> reclaimScanBreakpoint{nullptr};
#endif
}

SMRThread::SMRThread()
{
    std::lock_guard<std::mutex> lock(State().mutex);
    State().executors.push_back(this);
}
SMRThread::~SMRThread()
{
    CHECK_DETAIL(listPtr == nullptr && hazard.load() == 0, "thread exited with an SMR handle");
    std::lock_guard<std::mutex> lock(State().mutex);
    State().executors.remove(this);
}

#if defined(MRT_TESTABLE_INTERNALS)
void ThreadsSMRSupport::SetReclaimScanBreakpoint(void (*callback)())
{
    reclaimScanBreakpoint.store(callback);
}
#endif

bool ThreadsList::includes(const Mutator* thread) const
{
    return std::find(threads.begin(), threads.end(), thread) != threads.end();
}

ThreadsList* ThreadsSMRSupport::get_java_thread_list() { return State().current.load(); }

// threadSMR.cpp:876,1041: publish a new immutable membership list, then
// retire the old list. Registration never holds its lock across consumers.
void ThreadsSMRSupport::add_thread(Mutator* thread)
{
    std::lock_guard<std::mutex> lock(State().mutex);
    auto* old = get_java_thread_list();
    CHECK_DETAIL(!old->includes(thread), "thread already registered");
    auto* next = new ThreadsList;
    next->threads = old->threads;
    next->threads.push_back(thread);
    State().current.store(next);
    free_list(old);
}

void ThreadsSMRSupport::remove_thread(Mutator* thread)
{
    std::lock_guard<std::mutex> lock(State().mutex);
    auto* old = get_java_thread_list();
    CHECK_DETAIL(old->includes(thread), "thread not registered");
    auto* next = new ThreadsList;
    for (auto* entry : old->threads) {
        if (entry != thread) { next->threads.push_back(entry); }
    }
    State().current.store(next);
    free_list(old);
}

// threadSMR.cpp:912: only lists absent from hazards and nested handles can
// be reclaimed. Tagged hazards participate by address, without dereference.
void ThreadsSMRSupport::free_list(ThreadsList* list)
{
    auto& state = State();
    if (list != nullptr && list != &state.bootstrap) {
        list->next = state.retired;
        state.retired = list;
    }
    // threadSMR.cpp:932-938: gather every hazard ptr first, then an acquire
    // barrier, and only then read the nested reference counters. Reading the
    // counters before the hazards can miss a nested handoff that bumps the
    // counter after we read it but before we scan the hazard.
    std::vector<uintptr_t> hazards;
    hazards.reserve(state.executors.size());
    for (auto* executor : state.executors) {
        hazards.push_back(executor->hazard.load() & ~TAG);
    }
#if defined(MRT_TESTABLE_INTERNALS)
    if (auto breakpoint = reclaimScanBreakpoint.load()) { breakpoint(); }
#endif
    std::atomic_thread_fence(std::memory_order_acquire);
    for (auto** link = &state.retired; *link != nullptr;) {
        auto* candidate = *link;
        bool protectedList = candidate->nestedHandleCount.load() != 0;
        for (auto hazard : hazards) {
            protectedList |= hazard == reinterpret_cast<uintptr_t>(candidate);
        }
        if (protectedList) {
            link = &candidate->next;
        } else {
            *link = candidate->next;
            delete candidate;
        }
    }
}

// Called under the registration/deletion lock, as in threadSMR.cpp:985.
bool ThreadsSMRSupport::is_a_protected_JavaThread(Mutator* target)
{
    for (auto* executor : State().executors) {
        uintptr_t hazard = executor->hazard.load();
        if ((hazard & TAG) != 0) {
            // Win against acquisition and invalidate, or observe its verified
            // pointer. An unverified pointer must never be dereferenced.
            if (executor->hazard.compare_exchange_strong(hazard, 0)) { continue; }
        }
        if (hazard != 0 && (hazard & TAG) == 0 &&
            reinterpret_cast<ThreadsList*>(hazard)->includes(target)) { return true; }
    }
    for (auto* list = State().retired; list != nullptr; list = list->next) {
        if (list->nestedHandleCount.load() != 0 && list->includes(target)) { return true; }
    }
    return false;
}

void ThreadsSMRSupport::wait_until_not_protected(Mutator* thread)
{
    auto& state = State();
    std::unique_lock<std::mutex> lock(state.mutex);
    state.released.wait(lock, [&] { return !is_a_protected_JavaThread(thread); });
}

void ThreadsSMRSupport::smr_delete(Mutator* thread)
{
    wait_until_not_protected(thread);
    delete thread;
}

void ThreadsSMRSupport::release_stable_list_wake_up()
{
    std::lock_guard<std::mutex> lock(State().mutex);
    free_list(nullptr);
    State().released.notify_all();
}

SafeThreadsListPtr::SafeThreadsListPtr() : thread(&CurrentExecutor()) { acquire_stable_list(); }
SafeThreadsListPtr::~SafeThreadsListPtr() { release_stable_list(); }

void SafeThreadsListPtr::acquire_stable_list()
{
    previous = thread->listPtr;
    thread->listPtr = this;
    if (thread->hazard.load() == 0 && previous == nullptr) {
        acquire_stable_list_fast_path();
    } else {
        acquire_stable_list_nested_path();
    }
}

// threadSMR.cpp:433-478: publish tagged, validate current, verify by CAS.
void SafeThreadsListPtr::acquire_stable_list_fast_path()
{
    for (;;) {
        auto* list = ThreadsSMRSupport::get_java_thread_list();
        uintptr_t tagged = reinterpret_cast<uintptr_t>(list) | TAG;
        thread->hazard.store(tagged);
        if (ThreadsSMRSupport::get_java_thread_list() != list) { continue; }
        if (thread->hazard.compare_exchange_strong(tagged, reinterpret_cast<uintptr_t>(list))) {
            heldList = list;
            return;
        }
    }
}

void SafeThreadsListPtr::acquire_stable_list_nested_path()
{
    if (!previous->hasRefCount) {
        previous->heldList->nestedHandleCount.fetch_add(1);
        previous->hasRefCount = true;
    }
    thread->hazard.store(0);
    acquire_stable_list_fast_path();
}

void SafeThreadsListPtr::release_stable_list()
{
    CHECK_DETAIL(thread->listPtr == this, "SMR handles must be released in stack order");
    thread->listPtr = previous;
    thread->hazard.store(0);
    if (hasRefCount) { heldList->nestedHandleCount.fetch_sub(1); }
    ThreadsSMRSupport::release_stable_list_wake_up();
}
} // namespace MapleRuntime
