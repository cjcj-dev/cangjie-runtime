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

    void enter_safe();
    void leave_safe();
    bool observed_safe() const { return inSafe_.load(std::memory_order_acquire) != 0; }

private:
    // HotSpot handshake.hpp:115-122.
    class MatchOp {
    public:
        explicit MatchOp(HandshakeOperation* op) : op_(op) {}
        bool operator()(HandshakeOperation* op) { return op == op_; }

    private:
        HandshakeOperation* op_;
    };

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
