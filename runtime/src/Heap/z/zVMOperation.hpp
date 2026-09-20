#ifndef MRT_Z_VM_OPERATION_HPP
#define MRT_Z_VM_OPERATION_HPP

namespace MapleRuntime {

class VM_ZOperation {
public:
    virtual ~VM_ZOperation() = default;
    virtual bool do_operation() = 0;
    virtual bool block_jni_critical() const { return false; }
    bool pause();
};

} // namespace MapleRuntime

#endif
