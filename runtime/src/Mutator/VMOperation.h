// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#ifndef MRT_VM_OPERATION_H
#define MRT_VM_OPERATION_H

namespace MapleRuntime {
// HotSpot runtime/vmOperation.hpp: safepoint processing policy is a virtual
// property of the operation, not a property of the safepoint caller.
class VMOperation {
public:
    virtual ~VMOperation() = default;
    virtual bool skip_thread_oop_barriers() const { return false; }
};

// HotSpot runtime/vmThread.hpp:105-107,140. The STW execution lock serializes
// publication and clearing of the operation currently being executed.
class VMThread {
public:
    static VMOperation* vm_operation() { return currentOperation; }
private:
    friend class MutatorManager;
    static VMOperation* currentOperation;
};
} // namespace MapleRuntime
#endif
