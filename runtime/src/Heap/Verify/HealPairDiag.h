#ifndef MRT_HEAL_PAIR_DIAG_H
#define MRT_HEAL_PAIR_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
namespace HealPairDiag {

bool Enabled();

void NoteRaw(const void* oldAddr, const void* newAddr, const void* slot, uint16_t site);

void NoteCollect(uintptr_t start, uintptr_t end, uint64_t liveBytes, uint32_t rtype, uint32_t knownEmpty);

void NoteCrashRdi(uintptr_t rdi);

void NoteCrashRegs(uintptr_t rdi, uintptr_t rax, uintptr_t r12, uintptr_t r14, uintptr_t rbp);

void NoteCopy(const void* fromAddr, const void* toAddr, size_t size, uint32_t done);

// Default off. When MRT_GCV2_COPYSTALL_NS>0 and size>=min, pause after the tip word.
uint64_t MidCopyStallNs();
void MaybeMidCopyStall(size_t size);

void NoteZeroWrite(const void* slot, uintptr_t oldRaw, uintptr_t newRaw, uint16_t site);

void Report(const char* point);

} // namespace HealPairDiag
} // namespace MapleRuntime

#endif
