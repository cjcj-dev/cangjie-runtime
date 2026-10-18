#pragma once
#include "gc_unittest.hpp"
#include "Mutator/VMOperation.h"
#include <exception>
#include <utility>

namespace MapleRuntime::GcUnit {
// HotSpot vmOperations.hpp:87-93: test pauses use the product VM executor.
class VM_GTestExecuteAtSafepoint : public VMOperation {
public:
    explicit VM_GTestExecuteAtSafepoint(const char* reason) : reason(reason) {}
    const char* name() const override { return reason; }
private:
    const char* reason;
};

template<class Body>
class VM_GTestBody final : public VM_GTestExecuteAtSafepoint {
public:
    VM_GTestBody(const char* reason, Body body)
        : VM_GTestExecuteAtSafepoint(reason), body(std::move(body)) {}
    void doit() override
    {
        reached = true;
        onVM = VMThread::is_VM_thread();
        try { body(); } catch (...) { failure = std::current_exception(); }
    }
    Body body;
    std::exception_ptr failure;
    bool reached = false;
    bool onVM = false;
};

template<class Body>
void ExecuteTestAtSafepoint(const char* reason, Body body)
{
    VM_GTestBody<Body> operation(reason, std::move(body));
    VMThread::execute(&operation);
    std::fprintf(stderr, "VM1351_BODY reason=%s reached=%d vm=%d\n",
                 reason, operation.reached, operation.onVM);
    GC_EXPECT_TRUE(operation.reached);
    GC_EXPECT_TRUE(operation.onVM);
    if (operation.failure) { std::rethrow_exception(operation.failure); }
}
} // namespace MapleRuntime::GcUnit
