// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#ifndef MRT_Z_TASK_QUEUE_HPP
#define MRT_Z_TASK_QUEUE_HPP

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>
#include <type_traits>

namespace MapleRuntime {
// HotSpot gc/shared/taskqueue.hpp:333-429 and taskqueue.inline.hpp:110-287.
// One owner pushes/pops the bottom; thieves claim the top with a tagged CAS.
template<unsigned N>
class TaskQueueSuper {
protected:
    static_assert((N & (N - 1)) == 0, "power of two queue");
    static constexpr uint32_t MASK = N - 1;
    using Index = std::conditional_t<sizeof(void*) == 8, uint32_t, uint16_t>;
    static constexpr unsigned TAG_SHIFT = sizeof(Index) * 8;
    struct Age {
        Index top;
        Index tag;
        Age(uint32_t top, uint32_t tag) : top(static_cast<Index>(top)), tag(static_cast<Index>(tag)) {}
    };
    static uintptr_t pack(Age age) { return (uintptr_t(age.tag) << TAG_SHIFT) | age.top; }
    static Age unpack(uintptr_t age) { return { uint32_t(age), uint32_t(age >> TAG_SHIFT) }; }
    static uint32_t clean_size(uint32_t bottom, uint32_t top)
    {
        const uint32_t size = (bottom - top) & MASK;
        return size == MASK ? 0 : size;
    }
    alignas(64) std::atomic<uint32_t> bottom { 0 };
    alignas(64) std::atomic<uintptr_t> age { 0 };
public:
    enum class PopResult { Empty, Contended, Success };
    unsigned size() const
    {
        return clean_size(bottom.load(std::memory_order_relaxed), unpack(age.load(std::memory_order_relaxed)).top);
    }
    bool is_empty() const { return size() == 0; }
};

template<class E, unsigned N = (sizeof(void*) == 8 ? 1 << 17 : 1 << 14)>
class GenericTaskQueue : public TaskQueueSuper<N> {
    using Super = TaskQueueSuper<N>;
    using Age = typename Super::Age;
    using Super::MASK;
    using Super::pack;
    using Super::unpack;
    using Super::clean_size;
    using Super::bottom;
    using Super::age;
    std::unique_ptr<E[]> elems { new E[N] };
    unsigned lastStolen = unsigned(-1);
    int seed = 17;

    bool pop_local_slow(uint32_t localBottom, uintptr_t oldAge)
    {
        const Age old = unpack(oldAge);
        const uintptr_t next = pack({ localBottom, old.tag + 1 });
        if (localBottom == old.top && age.compare_exchange_strong(oldAge, next)) {
            return true;
        }
        age.store(next, std::memory_order_relaxed);
        return false;
    }
public:
    using element_type = E;
    using PopResult = typename Super::PopResult;
    bool push(E task)
    {
        const uint32_t b = bottom.load(std::memory_order_relaxed);
        const uint32_t top = unpack(age.load(std::memory_order_relaxed)).top;
        if (((b - top) & MASK) >= N - 2) return false;
        elems[b] = task;
        bottom.store((b + 1) & MASK, std::memory_order_release);
        return true;
    }
    bool pop_local(E& task)
    {
        uint32_t b = bottom.load(std::memory_order_relaxed);
        if (((b - unpack(age.load(std::memory_order_relaxed)).top) & MASK) == 0) return false;
        b = (b - 1) & MASK;
        bottom.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        task = elems[b];
        if (clean_size(b, unpack(age.load(std::memory_order_relaxed)).top) > 0) return true;
        std::atomic_thread_fence(std::memory_order_acquire);
        return pop_local_slow(b, age.load(std::memory_order_relaxed));
    }
    PopResult pop_global(E& task)
    {
        uintptr_t oldAge = age.load(std::memory_order_relaxed);
        const Age old = unpack(oldAge);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const uint32_t b = bottom.load(std::memory_order_acquire);
        if (clean_size(b, old.top) == 0) return PopResult::Empty;
        // As in HotSpot taskqueue.inline.hpp:232-252, a losing thief discards
        // the possibly concurrently overwritten trivially-copyable element.
        task = elems[old.top];
        const uint32_t top = (old.top + 1) & MASK;
        const uintptr_t next = pack({ top, old.tag + (top == 0 ? 1U : 0U) });
        return age.compare_exchange_strong(oldAge, next) ? PopResult::Success : PopResult::Contended;
    }
    bool is_last_stolen_queue_id_valid() const { return lastStolen != unsigned(-1); }
    unsigned last_stolen_queue_id() const { return lastStolen; }
    void set_last_stolen_queue_id(unsigned id) { lastStolen = id; }
    void invalidate_last_stolen_queue_id() { lastStolen = unsigned(-1); }
    unsigned next_random_queue_id()
    {
        const int value = 16807 * (seed % 127773) - 2836 * (seed / 127773);
        seed = value > 0 ? value : value + 2147483647;
        return unsigned(seed);
    }
};

// HotSpot taskqueue.hpp:437-465: overflow is owner-only, never stolen.
template<class E>
class OverflowTaskQueue : public GenericTaskQueue<E> {
    std::vector<E> overflow;
public:
    bool push(E task)
    {
        if (!GenericTaskQueue<E>::push(task)) overflow.push_back(task);
        return true;
    }
    bool pop_overflow(E& task)
    {
        if (overflow.empty()) return false;
        task = overflow.back();
        overflow.pop_back();
        return true;
    }
    bool is_empty() const { return GenericTaskQueue<E>::is_empty() && overflow.empty(); }
};

class TaskQueueSetSuper {
public:
    virtual unsigned tasks() const = 0;
    virtual ~TaskQueueSetSuper() = default;
};

// HotSpot taskqueue.inline.hpp:315-391: best of two, remembered victim,
// two-worker direct steal, and bounded retries.
template<class Q>
class GenericTaskQueueSet : public TaskQueueSetSuper {
    std::vector<Q*> queues;
    using E = typename Q::element_type;
    using PopResult = typename Q::PopResult;
    PopResult steal_best_of_2(unsigned self, E& task)
    {
        Q* local = queue(self);
        if (size() > 2) {
            unsigned k1 = self;
            if (local->is_last_stolen_queue_id_valid()) k1 = local->last_stolen_queue_id();
            else while (k1 == self) k1 = local->next_random_queue_id() % size();
            unsigned k2 = self;
            while (k2 == self || k2 == k1) k2 = local->next_random_queue_id() % size();
            const unsigned s1 = queue(k1)->size();
            const unsigned s2 = queue(k2)->size();
            unsigned selected = 0;
            PopResult result = PopResult::Empty;
            if (s2 > s1) {
                selected = k2;
                result = queue(k2)->pop_global(task);
            } else if (s1 > 0) {
                selected = k1;
                result = queue(k1)->pop_global(task);
            }
            if (result == PopResult::Success) local->set_last_stolen_queue_id(selected);
            else local->invalidate_last_stolen_queue_id();
            return result;
        }
        if (size() == 2) return queue((self + 1) % 2)->pop_global(task);
        return PopResult::Empty;
    }
public:
    explicit GenericTaskQueueSet(unsigned n) : queues(n, nullptr) {}
    void register_queue(unsigned id, Q* q) { queues[id] = q; }
    Q* queue(unsigned id) const { return queues[id]; }
    unsigned size() const { return queues.size(); }
    unsigned tasks() const override
    {
        unsigned count = 0;
        for (Q* q : queues) count += q->size();
        return count;
    }
    bool steal(unsigned self, E& task)
    {
        for (unsigned i = 0; i < 2 * size(); ++i) {
            if (steal_best_of_2(self, task) == PopResult::Success) return true;
        }
        return false;
    }
};
} // namespace MapleRuntime
#endif
