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
