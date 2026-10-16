#pragma once
#include "Mutator/VMOperation.h"
#include <functional>

namespace MapleRuntime::GcUnit {
// HotSpot VM_GTestExecute shape: the test asks the product VMThread to run a
// real producer on its native GC owner. No product function is recompiled.
class VMTestProducer final : public VMOperation {
public:
    explicit VMTestProducer(std::function<void()> producer) : producer(std::move(producer)) {}
    const char* name() const override { return "GC test native producer"; }
    bool evaluate_at_safepoint() const override { return false; }
    void doit() override { producer(); }
private:
    std::function<void()> producer;
};
inline void ProduceOnVMThread(std::function<void()> producer)
{
    VMTestProducer operation(std::move(producer));
    VMThread::execute(&operation);
}
}
