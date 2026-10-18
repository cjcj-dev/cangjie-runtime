// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#ifndef MRT_GC_VM_OPERATIONS_HPP
#define MRT_GC_VM_OPERATIONS_HPP

#include "Mutator/VMOperation.h"
#include "Heap/z/zDriverPort.hpp"

namespace MapleRuntime {
class VM_Heap_Sync_Operation : public VMOperation {
public:
    bool doit_prologue() override;
    void doit_epilogue() override;
};

class VM_GC_Operation : public VM_Heap_Sync_Operation {
public:
    VM_GC_Operation(uint32_t gcCountBefore, bool full, uint32_t fullGCCountBefore, GCReason cause);
    bool doit_prologue() override;
    void doit_epilogue() override;
    bool allow_nested_vm_operations() const override { return true; }
    bool prologue_succeeded() const { return prologueSucceeded; }
    bool is_shutting_down() const { return shuttingDown; }
protected:
    virtual bool skip_operation() const;
    const GCReason gcCause;
private:
    const uint32_t gcCountBefore;
    const uint32_t fullGCCountBefore;
    const bool full;
    bool prologueSucceeded = false;
    bool shuttingDown = false;
};
} // namespace MapleRuntime
#endif
