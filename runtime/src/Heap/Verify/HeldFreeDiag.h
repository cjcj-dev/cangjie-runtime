// ⛔ HOLLOWED — the implementation in the matching .cpp is all no-ops: Enabled() returns false and
// every sink body is empty.  The gate documented below therefore emits nothing, so a zero taken from
// it is a false negative, not evidence that the arm never fires.  The contract, the gate name and the
// product call sites were all left intact when the bodies were removed, which is precisely what makes
// this readable as a live instrument.  Restore the sink you need first -- PermWhoAdmit.cpp shows the
// shape: a compile-time constant gate (the campaign cut MRT_GCV2_* from 190 to 3) plus a line on the
// zero case so a zero cannot be read as a dead probe.  Guard: runtime/tests/check_diag_not_hollow.py
#ifndef MRT_HELD_FREE_DIAG_H
#define MRT_HELD_FREE_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;

// heldfree: was the crash slot enumerated / marked / fixed in the last young GC?
// Gate: MRT_GCV2_HELDFREE=1 or MRT_GCV2_DIAG token heldfree. Default off.
namespace HeldFreeDiag {

bool Enabled();

void BeginYoungCycle();

void NoteEnumSlot(const void* slot, BaseObject* target, const char* site);
void NotePush(BaseObject* object, const char* site);
void NoteMark(BaseObject* object);
void NoteFixSlot(const void* slot, BaseObject* target, int wrote, const char* site);

void NoteClearRange(uintptr_t start, std::size_t size);

void NoteCrashRegs(uintptr_t rax, uintptr_t rbx, uintptr_t rcx, uintptr_t rdx, uintptr_t rsi, uintptr_t rdi,
                   uintptr_t r12, uintptr_t r14, uintptr_t rbp);

void Report(const char* point);

} // namespace HeldFreeDiag
} // namespace MapleRuntime

#endif
