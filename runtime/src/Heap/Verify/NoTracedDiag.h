// ⛔ HOLLOWED — the implementation in the matching .cpp is all no-ops: Enabled() returns false and
// every sink body is empty.  The gate documented below therefore emits nothing, so a zero taken from
// it is a false negative, not evidence that the arm never fires.  The contract, the gate name and the
// product call sites were all left intact when the bodies were removed, which is precisely what makes
// this readable as a live instrument.  Restore the sink you need first -- PermWhoAdmit.cpp shows the
// shape: a compile-time constant gate (the campaign cut MRT_GCV2_* from 190 to 3) plus a line on the
// zero case so a zero cannot be read as a dead probe.  Guard: runtime/tests/check_diag_not_hollow.py
#ifndef MRT_NO_TRACED_DIAG_H
#define MRT_NO_TRACED_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;

// notraced: was TraceObjectRefFields ever called for a holder?
// Gate: MRT_GCV2_NOTRACED=1 or MRT_GCV2_DIAG token "notraced". Default off.
// Records young-only entries at TraceObjectRefFields entry; copy-remaps for
// move-independent crash join (same idea as whozero zero-ring remap).
namespace NoTracedDiag {

bool Enabled();

// TraceObjectRefFields entry (hot path when on; young-only ring write).
void NoteTrace(BaseObject* obj);

// Object copy: remap ring addrs from→to when done!=0 (mirror HealPairDiag whozero).
void NoteCopy(const void* fromAddr, const void* toAddr, size_t size, uint32_t done);

// Crash join: match holder identity against trace ring.
// Prefer CAS-null-time hObj when available; crash-face r13 is fallback.
void NoteCrashJoin(uintptr_t holderCrash, uintptr_t holderCas);

void Report(const char* point);

} // namespace NoTracedDiag
} // namespace MapleRuntime

#endif
