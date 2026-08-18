// ⛔ HOLLOWED — the implementation in the matching .cpp is all no-ops: Enabled() returns false and
// every sink body is empty.  The gate documented below therefore emits nothing, so a zero taken from
// it is a false negative, not evidence that the arm never fires.  The contract, the gate name and the
// product call sites were all left intact when the bodies were removed, which is precisely what makes
// this readable as a live instrument.  Restore the sink you need first -- PermWhoAdmit.cpp shows the
// shape: a compile-time constant gate (the campaign cut MRT_GCV2_* from 190 to 3) plus a line on the
// zero case so a zero cannot be read as a dead probe.  Guard: runtime/tests/check_diag_not_hollow.py
#ifndef MRT_HEAL_PAIR_DIAG_H
#define MRT_HEAL_PAIR_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
namespace HealPairDiag {

bool Enabled();

// youngclaim: distinguish which marking engine performed a 0->1 claim, then
// join a young claim with the major wasMarked=true field-walk skip. Default off.
// Gate: MRT_GCV2_YOUNGCLAIM=1 or the "youngclaim" diagnostic token.
bool YoungClaimEnabled();

class ScopedMajorMarkTask {
public:
    ScopedMajorMarkTask();
    ~ScopedMajorMarkTask();

    ScopedMajorMarkTask(const ScopedMajorMarkTask&) = delete;
    ScopedMajorMarkTask& operator=(const ScopedMajorMarkTask&) = delete;

private:
    bool active = false;
};

void NoteRaw(const void* oldAddr, const void* newAddr, const void* slot, uint16_t site);

void NoteCollect(uintptr_t start, uintptr_t end, uint64_t liveBytes, uint32_t rtype, uint32_t knownEmpty);

void NoteCrashRdi(uintptr_t rdi);

void NoteCrashRegs(uintptr_t rdi, uintptr_t rax, uintptr_t r12, uintptr_t r14, uintptr_t rbp);

void NoteCopy(const void* fromAddr, const void* toAddr, size_t size, uint32_t done);

// Default off. When MRT_GCV2_COPYSTALL_NS>0 and size>=min, pause after the tip word.
uint64_t MidCopyStallNs();
void MaybeMidCopyStall(size_t size);

void NoteZeroWrite(const void* slot, uintptr_t oldRaw, uintptr_t newRaw, uint16_t site);

// edgemiss: non-zero heap-slot writes that install a large-region target (default off).
// Gate: MRT_GCV2_EDGEMISS=1 or MRT_GCV2_WHOZERO=1. Remap-on-copy like zero ring.
// barrierKind: 1=Idle 2=Enum 3=Trace 4=PostTrace 5=Preforward 6=Forward 7=STW 0=unknown
void NoteEdgeWrite(const void* holder, const void* slot, uintptr_t oldRaw, uintptr_t newRaw,
                   uint8_t barrierKind);

// First-claim mark of an object (0→1). youngClaim is true only for the
// TraceYoungClosure claim path; major-task origin is supplied by the scope above.
void NoteFirstMark(const void* obj, bool youngClaim = false);

// Rare major branch: MarkObject returned true, so ConcurrentMarkingWork skips
// TraceObjectRefFields. Records object/region/gc/phase and source-claim join.
void NoteMajorWasMarked(const void* obj);

// Periodic persistence point; caller invokes this at each GC end so timeout or
// SIGKILL cannot erase the activity/group counters already observed.
void ReportYoungClaim(const char* point);

// holdercapture: silent read of the claim / major-skip ledgers for one object.
// Returns the claim source (0 unknown, 1 young, 2 major, 3 other) and fills the
// out-params from the major wasMarked=true skip record when one exists.
// Does not print and does not move the lookup-miss counter.
uint8_t LookupMarkOrigin(uintptr_t obj, uint32_t* claimGc, uint8_t* majorSkip, uint32_t* majorGc,
                         uint8_t* hasRef, uint8_t* nonYoungRef);

// whozero: crash-time match of LexerImpl-style null Array* (rcx=0) against zero-write ring.
// Gate: MRT_GCV2_WHOZERO=1 or MRT_GCV2_HEALPAIR / healpair token. Default off.
void NoteCrashWhoZero(uintptr_t r13, uintptr_t rcx, uintptr_t rsi, uintptr_t rbx, uintptr_t r12);

void Report(const char* point);

} // namespace HealPairDiag
} // namespace MapleRuntime

#endif
