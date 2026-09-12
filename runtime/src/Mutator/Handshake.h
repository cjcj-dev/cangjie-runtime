#ifndef MRT_HANDSHAKE_H
#define MRT_HANDSHAKE_H

#include <list>
#include <mutex>

namespace MapleRuntime {
struct ThreadLocalData;

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

private:
    ThreadLocalData* handshakee_;
    std::mutex lock_;
    std::list<HandshakeOperation*> queue_;
};

class Handshake {
public:
    static HandshakeState& Current();
    static HandshakeState* ForTls(ThreadLocalData* tls);
    static void execute(HandshakeClosure* cl);
};

void ArmThreadPoll(ThreadLocalData* tls);
void UpdatePollValues(ThreadLocalData* tls);
bool HasPendingSafepoint(ThreadLocalData* tls);
} // namespace MapleRuntime
#endif
