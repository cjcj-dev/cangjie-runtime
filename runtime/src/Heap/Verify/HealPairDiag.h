// Temporary zero-write/forwarded blocker instrument. Only the live whozero arm is
// retained; the unrelated hollowed contracts and their product call sites were removed.
#ifndef MRT_HEAL_PAIR_DIAG_H
#define MRT_HEAL_PAIR_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
namespace HealPairDiag {

void NoteZeroWrite(const void* slot, uintptr_t oldRaw, uintptr_t newRaw, uint16_t site);

// whozero: crash-time match of LexerImpl-style null Array* (rcx=0) against zero-write ring.
// Gate: MRT_GCV2_WHOZERO=1 or MRT_GCV2_HEALPAIR / healpair token. Default off.
void NoteCrashWhoZero(uintptr_t r13, uintptr_t rcx, uintptr_t rsi, uintptr_t rbx, uintptr_t r12);

void Report(const char* point);

} // namespace HealPairDiag
} // namespace MapleRuntime

#endif
