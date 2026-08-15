#ifndef MRT_STACK_ROOT_SLOT_ATTEST_H
#define MRT_STACK_ROOT_SLOT_ATTEST_H

#include <cstddef>
#include <cstdint>

#include "StackMap/StackMapTypeDef.h"

namespace MapleRuntime {
class Mutator;

// Per-managed-frame observation of stack-map declarations versus visitor calls.
// Gate: MRT_GCV2_VERIFY_STACK_ROOT_SLOTS=1. Report-only; never changes root state.
namespace StackRootSlotAttest {

bool Enabled();
void Begin(const char* source);
void Finish();
bool FrameActive();

class FrameScope {
public:
    explicit FrameScope(size_t frameIndex);
    ~FrameScope();

    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;

private:
    size_t previousIndex_{ 0 };
    bool previousValid_{ false };
    bool engaged_{ false };
};

class SuppressScope {
public:
    SuppressScope();
    ~SuppressScope();

    SuppressScope(const SuppressScope&) = delete;
    SuppressScope& operator=(const SuppressScope&) = delete;

private:
    bool engaged_{ false };
};

void CheckFrame(uintptr_t framePC, bool mapValid, Mutator& mutator,
                const StackMapRootCounts& declared, const StackMapRootCounts& visited);

} // namespace StackRootSlotAttest
} // namespace MapleRuntime

#endif // MRT_STACK_ROOT_SLOT_ATTEST_H
