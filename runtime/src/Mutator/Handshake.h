#ifndef MRT_HANDSHAKE_H
#define MRT_HANDSHAKE_H

#include <atomic>
#include <mutex>
#include <vector>

#include "FilterQueue.h"

namespace MapleRuntime {
struct ThreadLocalData;
class Mutator;

class HandshakeClosure {
public:
    explicit HandshakeClosure(const char* name) : name_(name) {}
    virtual ~HandshakeClosure() = default;
    const char* name() const { return name_; }
    virtual void do_thread(Mutator* thread) = 0;

private:
    const char* name_;
};

class HandshakeOperation {
public:
    HandshakeOperation(HandshakeClosure* cl, Mutator* target);
    void prepare(Mutator* target, Mutator* executingThread);
    HandshakeClosure* closure() const { return cl_; }
    Mutator* target() const { return target_; }
    void do_handshake(Mutator* thread);
    void add_target_count(size_t count) { pending_.fetch_add(static_cast<int>(count), std::memory_order_relaxed); }
    bool is_completed() const { return pending_.load(std::memory_order_acquire) == 0; }

private:
    HandshakeClosure* cl_;
    Mutator* target_;
    Mutator* requester_;
    std::atomic<int> pending_{1};
};

class HandshakeState {
public:
    explicit HandshakeState(Mutator* handshakee) : handshakee_(handshakee) {}
    ~HandshakeState();

    Mutator* handshakee() const { return handshakee_; }

    void add_operation(HandshakeOperation* op);
    // HotSpot handshake.hpp:130: a lock-free acquire of the queue head.
    bool has_operation() { return !queue_.is_empty(); }
    bool operation_pending(HandshakeOperation* op);
    HandshakeOperation* get_op_for_self();
    HandshakeOperation* get_op();
    void remove_op(HandshakeOperation* op);
    bool process_by_self();
    bool try_process();
    bool claim_handshake();
    bool possibly_can_process();

    // HotSpot interfaceSupport.inline.hpp:87-91: entering native is a state
    // store. A handshaker that still reads the old state only declines.
    void enter_safe() { inSafe_.store(1, std::memory_order_release); }
    // HotSpot interfaceSupport.inline.hpp:96-108, sharedRuntime_x86_64.cpp:2466-2480:
    // publish the unsafe state, then look for a request, both in the single
    // total order. HotSpot reads the poll word that add_operation armed; this
    // reads the queue that the poll word stands for. An empty queue means every
    // later push is followed by a read of inSafe_ == 0 in try_process, and every
    // earlier operation was removed after it ran. Otherwise a handshaker may
    // already own lock_ and be executing for this thread, so wait for it.
    void leave_safe()
    {
        inSafe_.store(0, std::memory_order_seq_cst);
        if (!queue_.is_empty_seq_cst()) {
            leave_safe_slow();
        }
    }
    bool observed_safe() const { return inSafe_.load(std::memory_order_seq_cst) != 0; }

private:
    // HotSpot handshake.hpp:115-122.
    class MatchOp {
    public:
        explicit MatchOp(HandshakeOperation* op) : op_(op) {}
        bool operator()(HandshakeOperation* op) { return op == op_; }

    private:
        HandshakeOperation* op_;
    };

    void leave_safe_slow();

    Mutator* const handshakee_;
    std::mutex lock_;
    // HotSpot handshake.hpp:98-101: adds are lock free; peek, pop and contains
    // stay serialized by lock_.
    FilterQueue<HandshakeOperation*> queue_;
    std::atomic<int> inSafe_ = { 1 };
};

class Handshake {
public:
    static HandshakeState& Current();
    static void execute(HandshakeClosure* cl);
    static void execute(HandshakeClosure* cl, Mutator* target);
};

void ArmThreadPoll(ThreadLocalData* tls);
void ArmAllThreadPolls();
void PublishCpuProfileRequest(Mutator* mutator);
void ConsumeCpuProfileRequest(Mutator* mutator);
bool ClaimCpuProfileRequest(Mutator* mutator);
void CompleteCpuProfileRequest(Mutator* mutator);
bool CpuProfileRequestQueued(const Mutator* mutator);
void ProcessSafepointIfRequested(ThreadLocalData* tls);
void UpdatePollValues(ThreadLocalData* tls);
bool HasPendingSafepoint(ThreadLocalData* tls);
bool GlobalPoll();
bool HasPendingCpuProfileRequest();
} // namespace MapleRuntime
#endif
