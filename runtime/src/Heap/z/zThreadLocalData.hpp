#pragma once
#include <memory>
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zMarkStack.hpp"
namespace MapleRuntime {
// ZThreadLocalData: one store buffer and two generation stacks per OS thread.
struct ThreadGCData {
    StoreBarrierBuffer storeBarrierBuffer;
    std::unique_ptr<MarkThreadLocalStacks> markStacks[2];
};

}
