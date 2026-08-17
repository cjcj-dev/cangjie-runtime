#include "Heap/Verify/HeldFreeDiag.h"
namespace MapleRuntime {
namespace HeldFreeDiag {
bool Enabled() { return false; }
void BeginYoungCycle() {  }
void NoteEnumSlot(const void* slot, BaseObject* target, const char* site) {  }
void NotePush(BaseObject* object, const char* site) {  }
void NoteMark(BaseObject* object) {  }
void NoteFixSlot(const void* slot, BaseObject* target, int wrote, const char* site) {  }
void NoteClearRange(uintptr_t start, std::size_t size) {  }
void NoteCrashRegs(uintptr_t rax, uintptr_t rbx, uintptr_t rcx, uintptr_t rdx, uintptr_t rsi, uintptr_t rdi, uintptr_t r12, uintptr_t r14, uintptr_t rbp) {  }
void Report(const char* point) {  }
} // namespace HeldFreeDiag
} // namespace MapleRuntime
