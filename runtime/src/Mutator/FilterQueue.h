#ifndef MRT_FILTER_QUEUE_H
#define MRT_FILTER_QUEUE_H

#include <atomic>
#include <chrono>
#include <thread>

namespace MapleRuntime {
// HotSpot utilities/spinYield.hpp:35-70 and spinYield.cpp:38-48: spin, then
// yield, then sleep, for the rare CAS that keeps failing.
class SpinYield {
public:
    static constexpr unsigned default_spin_limit = 4096;
    static constexpr unsigned default_yield_limit = 64;
    static constexpr unsigned default_sleep_ns = 1000;

    explicit SpinYield(unsigned spin_limit = default_spin_limit, unsigned yield_limit = default_yield_limit,
                       unsigned sleep_ns = default_sleep_ns)
        : spin_limit_(is_mp() ? spin_limit : 0),
          yield_limit_(yield_limit), sleep_ns_(sleep_ns) {}

    void wait()
    {
        if (spins_ < spin_limit_) {
            ++spins_;
            spin_pause();
        } else {
            yield_or_sleep();
        }
    }

private:
    // HotSpot os.hpp:327-336: os::is_MP() answers from a processor count that
    // is read once, and an unknown count is treated as MP. Asking the OS in
    // the constructor would put a system call into every push and pop.
    static bool is_mp()
    {
        static const bool mp = std::thread::hardware_concurrency() != 1;
        return mp;
    }

    static void spin_pause()
    {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        __asm__ volatile("yield");
#endif
    }

    void yield_or_sleep()
    {
        if (yields_ < yield_limit_) {
            ++yields_;
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns_));
        }
    }

    unsigned spins_ = 0;
    unsigned yields_ = 0;
    const unsigned spin_limit_;
    const unsigned yield_limit_;
    const unsigned sleep_ns_;
};

// HotSpot utilities/filterQueue.hpp:31-87 and filterQueue.inline.hpp:33-138.
// The FilterQueue is FIFO with the ability to skip over queued items.
// The skipping is controlled by using a filter when popping.
// It also supports lock free pushes, while popping (including contains())
// needs to be externally serialized.
// HotSpot's compare_set defaults to memory_order_conservative, a two-way
// barrier (atomicAccess.hpp:46-47). The compare-exchanges here are seq_cst
// operations and not fences: a caller that pairs a push with a store to
// another location, as HandshakeState::add_operation does, fences itself.
template<class E>
class FilterQueue {
private:
    class Node {
    public:
        explicit Node(const E& e) : next_(nullptr), data_(e) {}
        Node* next_;
        E data_;
    };

    std::atomic<Node*> first_;
    Node* load_first() { return first_.load(std::memory_order_acquire); }

    static bool match_all(E) { return true; }

public:
    FilterQueue() : first_(nullptr) {}
    FilterQueue(const FilterQueue&) = delete;
    FilterQueue& operator=(const FilterQueue&) = delete;

    bool is_empty() { return load_first() == nullptr; }

    // A read in the single total order of the seq_cst operations, for a caller
    // that has just made a seq_cst store to another location and must not miss
    // a push that precedes it there. The acquire read above gives no such
    // order: on AArch64 an stlr followed by a plain load is not a barrier.
    bool is_empty_seq_cst() { return first_.load(std::memory_order_seq_cst) == nullptr; }

    // Adds an item to the queue in a MT safe way, re-entrant.
    void push(E data)
    {
        Node* head;
        Node* insnode = new Node(data);
        SpinYield yield(SpinYield::default_spin_limit * 10); // Very unlikely with multiple failed CAS.
        while (true) {
            head = load_first();
            insnode->next_ = head;
            if (first_.compare_exchange_strong(head, insnode)) {
                break;
            }
            yield.wait();
        }
    }

    // Applies the match_func to the items in the queue until match_func returns
    // true and then returns true, or there is no more items and then returns
    // false. Items pushed after execution starts will not have match_func
    // applied. The method is not re-entrant and must be executed mutually
    // exclusive to other contains and pops calls.
    template<typename MATCH_FUNC>
    bool contains(MATCH_FUNC& match_func)
    {
        Node* cur = load_first();
        if (cur == nullptr) {
            return false;
        }
        do {
            if (match_func(cur->data_)) {
                return true;
            }
            cur = cur->next_;
        } while (cur != nullptr);
        return false;
    }

    // Same as peek(MATCH_FUNC& match_func) but matches everything, thus returning
    // the first inserted item.
    E peek() { return peek(match_all); }

    // Applies the match_func to each item in the queue and returns the first
    // inserted item for which match_func returns true. Returns false if there are
    // no matches or the queue is empty. Any pushed item before execution is
    // complete may or may not have match_func applied. The method is not
    // re-entrant and must be executed mutual exclusive to other contains and pops
    // calls.
    template<typename MATCH_FUNC>
    E pop(MATCH_FUNC& match_func)
    {
        Node* first = load_first();
        Node* cur = first;
        Node* prev = nullptr;
        Node* match = nullptr;
        Node* match_prev = nullptr;

        if (cur == nullptr) {
            return static_cast<E>(nullptr);
        }
        SpinYield yield(SpinYield::default_spin_limit * 10); // Very unlikely with multiple failed CAS.
        do {
            do {
                if (match_func(cur->data_)) {
                    match = cur;
                    match_prev = prev;
                }
                prev = cur;
                cur = cur->next_;
            } while (cur != nullptr);

            if (match == nullptr) {
                return static_cast<E>(nullptr);
            }

            if (match_prev == nullptr) {
                // Working on first
                Node* expected = match;
                if (first_.compare_exchange_strong(expected, match->next_)) {
                    E ret = match->data_;
                    delete match;
                    return ret;
                }
                yield.wait();
                // Failed, we need to restart to know the Node prior to the match.
                first = load_first();
                cur = first;
                prev = nullptr;
                match = nullptr;
                match_prev = nullptr;
            } else {
                match_prev->next_ = match->next_;
                E ret = match->data_;
                delete match;
                return ret;
            }
        } while (true);
    }

    template<typename MATCH_FUNC>
    E peek(MATCH_FUNC& match_func)
    {
        Node* first = load_first();
        Node* cur = first;
        Node* match = nullptr;

        if (cur == nullptr) {
            return static_cast<E>(nullptr);
        }
        do {
            if (match_func(cur->data_)) {
                match = cur;
            }
            cur = cur->next_;
        } while (cur != nullptr);

        if (match == nullptr) {
            return static_cast<E>(nullptr);
        }
        return match->data_;
    }
};
} // namespace MapleRuntime
#endif // MRT_FILTER_QUEUE_H
