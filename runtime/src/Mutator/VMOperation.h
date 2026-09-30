// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#ifndef MRT_VM_OPERATION_H
#define MRT_VM_OPERATION_H

#include <condition_variable>
#include <mutex>
#include <thread>

namespace MapleRuntime {
struct ThreadLocalData;
class VMOperation {
public:
    virtual ~VMOperation() = default;
    ThreadLocalData* calling_thread() const { return callingThread; }
    void set_calling_thread(ThreadLocalData* thread) { callingThread = thread; }
    void evaluate() { doit(); }
    virtual void doit() = 0;
    virtual bool doit_prologue() { return true; }
    virtual void doit_epilogue() {}
    virtual bool evaluate_at_safepoint() const { return true; }
    virtual bool allow_nested_vm_operations() const { return false; }
    virtual bool is_gc_operation() const { return false; }
    virtual bool skip_thread_oop_barriers() const { return false; }
    virtual const char* name() const = 0;
private:
    ThreadLocalData* callingThread = nullptr;
};

class VMThread {
public:
    static void create();
    static void wait_for_vm_thread_exit();
    static void execute(VMOperation* operation);
    static VMOperation* vm_operation();
    static bool is_VM_thread();
    static bool is_running();
    static bool is_terminated();
private:
    VMThread();
    static VMThread& instance();
    void run();
    void inner_execute(VMOperation* operation);
    void wait_until_executed(VMOperation* operation);
    bool set_next_operation(VMOperation* operation);
    std::mutex lock;
    std::condition_variable condition;
    std::thread thread;
    VMOperation* currentOperation = nullptr;
    VMOperation* nextOperation;
    bool running = false;
    bool shouldTerminate = false;
    bool terminated = false;
};
} // namespace MapleRuntime
#endif
