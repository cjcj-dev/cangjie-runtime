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

void NoteCrashRegs(uintptr_t rdi, uintptr_t rax, uintptr_t r12, uintptr_t r14);

void Report(const char* point);

} // namespace HealPairDiag
} // namespace MapleRuntime

#endif
