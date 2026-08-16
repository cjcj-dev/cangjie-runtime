#ifndef MRT_HEAL_PAIR_DIAG_H
#define MRT_HEAL_PAIR_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
namespace HealPairDiag {

bool Enabled();

// youngage: one cached gate shared by the minor histogram and the existing
// CopyObject NoteCopy hook.  Default off: MRT_GCV2_YOUNGAGE=1.
bool YoungAgeEnabled();

void NoteRaw(const void* oldAddr, const void* newAddr, const void* slot, uint16_t site);

void NoteCollect(uintptr_t start, uintptr_t end, uint64_t liveBytes, uint32_t rtype, uint32_t knownEmpty);

void NoteCrashRdi(uintptr_t rdi);

void NoteCrashRegs(uintptr_t rdi, uintptr_t rax, uintptr_t r12, uintptr_t r14, uintptr_t rbp);

void NoteCopy(const void* fromAddr, const void* toAddr, size_t size, uint32_t done);

// Default off. When MRT_GCV2_COPYSTALL_NS>0 and size>=min, pause after the tip word.
uint64_t MidCopyStallNs();
void MaybeMidCopyStall(size_t size);

void NoteZeroWrite(const void* slot, uintptr_t oldRaw, uintptr_t newRaw, uint16_t site);

// whozero: crash-time match of LexerImpl-style null Array* (rcx=0) against zero-write ring.
// Gate: MRT_GCV2_WHOZERO=1 or MRT_GCV2_HEALPAIR / healpair token. Default off.
void NoteCrashWhoZero(uintptr_t r13, uintptr_t rcx, uintptr_t rsi, uintptr_t rbx, uintptr_t r12);

void Report(const char* point);

} // namespace HealPairDiag
} // namespace MapleRuntime

#endif
