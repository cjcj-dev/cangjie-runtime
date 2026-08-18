// ⛔ HOLLOWED — the implementation in the matching .cpp is all no-ops: Enabled() returns false and
// every sink body is empty.  The gate documented below therefore emits nothing, so a zero taken from
// it is a false negative, not evidence that the arm never fires.  The contract, the gate name and the
// product call sites were all left intact when the bodies were removed, which is precisely what makes
// this readable as a live instrument.  Restore the sink you need first -- PermWhoAdmit.cpp shows the
// shape: a compile-time constant gate (the campaign cut MRT_GCV2_* from 190 to 3) plus a line on the
// zero case so a zero cannot be read as a dead probe.  Guard: runtime/tests/check_diag_not_hollow.py
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
