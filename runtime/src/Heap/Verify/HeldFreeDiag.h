#ifndef MRT_HELD_FREE_DIAG_H
#define MRT_HELD_FREE_DIAG_H

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

void NoteClearRange(uintptr_t start, size_t size);

void NoteCrashRegs(uintptr_t rax, uintptr_t rbx, uintptr_t rcx, uintptr_t rdx, uintptr_t rsi, uintptr_t rdi,
                   uintptr_t r12, uintptr_t r14, uintptr_t rbp);

void Report(const char* point);

} // namespace HeldFreeDiag
} // namespace MapleRuntime

#endif
