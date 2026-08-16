#ifndef MRT_GATE_DROP_DIAG_H
#define MRT_GATE_DROP_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;

// gatedrop: record TraceRefField gate rejects (default off).
// Gate: MRT_GCV2_GATEDROP=1 or MRT_GCV2_DIAG token "gatedrop".
// Only runs on reject arms — never on admit path. Does not change product gates.
namespace GateDropDiag {

// arm: 1=MarkGoodHeapGate (mark-good path) 2=Plausible (mark-good path)
//      3=Plausible (slow path)
enum Arm : uint8_t {
    ARM_MARKGOOD = 1,
    ARM_PLAUSIBLE_GOOD = 2,
    ARM_PLAUSIBLE_SLOW = 3,
};

bool Enabled();

// Record one silent drop (reject + early return, leave untraced).
// fieldPtr is &RefField<> for FieldOffset; may be null.
void NoteReject(BaseObject* holder, void* fieldPtr, BaseObject* target, uint8_t arm);

// Crash join: match whozeroExact target (oldRaw peel) against reject ring.
void NoteCrashJoin(uintptr_t holder, uintptr_t slotBytes, uintptr_t tgtPeeled);

void Report(const char* point);

} // namespace GateDropDiag
} // namespace MapleRuntime

#endif
