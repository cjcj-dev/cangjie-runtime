// ⛔ HOLLOWED — the implementation in the matching .cpp is all no-ops: Enabled() returns false and
// every sink body is empty.  The gate documented below therefore emits nothing, so a zero taken from
// it is a false negative, not evidence that the arm never fires.  The contract, the gate name and the
// product call sites were all left intact when the bodies were removed, which is precisely what makes
// this readable as a live instrument.  Restore the sink you need first -- PermWhoAdmit.cpp shows the
// shape: a compile-time constant gate (the campaign cut MRT_GCV2_* from 190 to 3) plus a line on the
// zero case so a zero cannot be read as a dead probe.  Guard: runtime/tests/check_diag_not_hollow.py
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
