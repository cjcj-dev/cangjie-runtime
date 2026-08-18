// ⛔ HOLLOWED — the implementation in the matching .cpp is all no-ops: Enabled() returns false and
// every sink body is empty.  The gate documented below therefore emits nothing, so a zero taken from
// it is a false negative, not evidence that the arm never fires.  The contract, the gate name and the
// product call sites were all left intact when the bodies were removed, which is precisely what makes
// this readable as a live instrument.  Restore the sink you need first -- PermWhoAdmit.cpp shows the
// shape: a compile-time constant gate (the campaign cut MRT_GCV2_* from 190 to 3) plus a line on the
// zero case so a zero cannot be read as a dead probe.  Guard: runtime/tests/check_diag_not_hollow.py
#ifndef MRT_ENUM_PUSH_DIAG_H
#define MRT_ENUM_PUSH_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;

// enumpush: did GcPhaseEnum / VisitHeapReferencesOnStack push a given stack-map root?
// Gate: MRT_GCV2_ENUMPUSH=1 or MRT_GCV2_DIAG token enumpush. Default off.
// Default-off path is a single static bool load; no output, no counters.
namespace EnumPushDiag {

bool Enabled();

void NoteFrame(uintptr_t startIP, uintptr_t frameIP, uintptr_t fa, int managed, int valid,
               int nSlots, int nRegs, const char* funcName);

void NoteMapSlot(uintptr_t fa, intptr_t bias, BaseObject* obj);
void NoteMapReg(uintptr_t fa, int reg, BaseObject* obj);

void NotePush(BaseObject* obj, const char* site, const void* slot);
void NoteSkip(BaseObject* obj, const char* site, const char* reason);

void Report(const char* point);

} // namespace EnumPushDiag
} // namespace MapleRuntime

#endif
