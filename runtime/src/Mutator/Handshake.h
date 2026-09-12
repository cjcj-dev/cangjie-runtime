#ifndef MRT_HANDSHAKE_H
#define MRT_HANDSHAKE_H

#include <atomic>
#include <list>
#include <mutex>
#include <vector>

namespace MapleRuntime {
struct ThreadLocalData;
class Mutator;

class HandshakeClosure {
public:
    explicit HandshakeClosure(const char* name) : name_(name) {}
    virtual ~HandshakeClosure() = default;
    const char* name() const { return name_; }
    virtual void do_thread(ThreadLocalData* tls) = 0;

private:
    const char* name_;
};

class HandshakeOperation {
public:
    HandshakeOperation(HandshakeClosure* cl, ThreadLocalData* target) : cl_(cl), target_(target) {}
    HandshakeClosure* closure() const { return cl_; }
    ThreadLocalData* target() const { return target_; }
    void do_handshake(ThreadLocalData* tls)
    {
        if (cl_ != nullptr) {
            cl_->do_thread(tls);
        }
    }

private:
    HandshakeClosure* cl_;
    ThreadLocalData* target_;
};

class HandshakeState {
public:
    explicit HandshakeState(ThreadLocalData* handshakee) : handshakee_(handshakee) {}

    void set_handshakee(ThreadLocalData* tls) { handshakee_ = tls; }
    ThreadLocalData* handshakee() const { return handshakee_; }

    void add_operation(HandshakeOperation* op);
    bool has_operation();
    bool operation_pending(HandshakeOperation* op);
    HandshakeOperation* get_op_for_self();
    HandshakeOperation* get_op();
    void remove_op(HandshakeOperation* op);
    void process_by_self();
    bool try_process();
    bool claim_handshake();
    bool possibly_can_process();
    void process_queued_then_detach(void (*flush)(ThreadLocalData*));

    void enter_safe();
    void leave_safe();
    bool observed_safe() const { return inSafe_.load(std::memory_order_acquire) != 0; }

private:
    ThreadLocalData* handshakee_;
    std::mutex lock_;
    std::list<HandshakeOperation*> queue_;
    std::atomic<int> inSafe_ = { 1 };
};

class Handshake {
public:
    static HandshakeState& Current();
    static void BindCurrent(HandshakeState* state);
    static HandshakeState* ForTls(ThreadLocalData* tls);
    static void execute(HandshakeClosure* cl);
    static void execute(HandshakeClosure* cl, ThreadLocalData* target);
};

void ArmThreadPoll(ThreadLocalData* tls);
void ArmAllThreadPolls();
void PublishCpuProfileRequest(Mutator* mutator);
void ConsumeCpuProfileRequest(Mutator* mutator);
bool ClaimCpuProfileRequest(Mutator* mutator);
void CompleteCpuProfileRequest(Mutator* mutator);
bool CpuProfileRequestQueued(const Mutator* mutator);
void UpdatePollValues(ThreadLocalData* tls);
bool HasPendingSafepoint(ThreadLocalData* tls);
bool GlobalPoll();
bool HasPendingCpuProfileRequest();
} // namespace MapleRuntime
#endif
