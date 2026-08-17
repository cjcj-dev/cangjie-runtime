#include "Heap/Verify/StackRootSlotAttest.h"
namespace MapleRuntime {
namespace StackRootSlotAttest {
bool Enabled() { return false; }
void Begin(const char* source) {  }
void Finish() {  }
bool FrameActive() { return false; }
FrameScope::FrameScope(size_t frameIndex) {}
FrameScope::~FrameScope() {}
SuppressScope::SuppressScope() {}
SuppressScope::~SuppressScope() {}
void CheckFrame(uintptr_t framePC, bool mapValid, Mutator& mutator, const StackMapRootCounts& declared, const StackMapRootCounts& visited) {  }
} // namespace StackRootSlotAttest
} // namespace MapleRuntime
