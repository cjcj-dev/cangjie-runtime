// ⛔ HOLLOWED — the implementation in the matching .cpp is all no-ops: Enabled() returns false and
// every sink body is empty.  The gate documented below therefore emits nothing, so a zero taken from
// it is a false negative, not evidence that the arm never fires.  The contract, the gate name and the
// product call sites were all left intact when the bodies were removed, which is precisely what makes
// this readable as a live instrument.  Restore the sink you need first -- PermWhoAdmit.cpp shows the
// shape: a compile-time constant gate (the campaign cut MRT_GCV2_* from 190 to 3) plus a line on the
// zero case so a zero cannot be read as a dead probe.  Guard: runtime/tests/check_diag_not_hollow.py
#ifndef MRT_PIN_FIRE_DIAG_H
#define MRT_PIN_FIRE_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;

// pinfire: count whether raw-pointer pin reclaim path ran / blocked.
// Gate: MRT_GCV2_PINFIRE=1 (exact). Default off. Report-only; no reclaim decision change.
namespace PinFireDiag {

bool Enabled();

void NoteAddRawPointer();
void NoteCollectPinnedGarbage();
// early-return from CollectFreePinnedSlots while count > 0
void NoteSkipFreeSlots(RegionInfo* region);
// whole-region skip in CollectPinnedGarbage while count > 0
void NoteSkipRegion(RegionInfo* region);

void Report(const char* point);

} // namespace PinFireDiag
} // namespace MapleRuntime

#endif
