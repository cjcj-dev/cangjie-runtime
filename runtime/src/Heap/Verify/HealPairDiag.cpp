#include "Heap/Verify/HealPairDiag.h"
namespace MapleRuntime {
namespace HealPairDiag {
bool Enabled() { return false; }
bool YoungClaimEnabled() { return false; }
ScopedMajorMarkTask::ScopedMajorMarkTask() {}
ScopedMajorMarkTask::~ScopedMajorMarkTask() {}
void NoteRaw(const void* oldAddr, const void* newAddr, const void* slot, uint16_t site) {  }
void NoteCollect(uintptr_t start, uintptr_t end, uint64_t liveBytes, uint32_t rtype, uint32_t knownEmpty) {  }
void NoteCrashRdi(uintptr_t rdi) {  }
void NoteCrashRegs(uintptr_t rdi, uintptr_t rax, uintptr_t r12, uintptr_t r14, uintptr_t rbp) {  }
void NoteCopy(const void* fromAddr, const void* toAddr, size_t size, uint32_t done) {  }
uint64_t MidCopyStallNs() { return 0; }
void MaybeMidCopyStall(size_t size) {  }
void NoteZeroWrite(const void* slot, uintptr_t oldRaw, uintptr_t newRaw, uint16_t site) {  }
void NoteEdgeWrite(const void* holder, const void* slot, uintptr_t oldRaw, uintptr_t newRaw, uint8_t barrierKind) {  }
void NoteFirstMark(const void* obj, bool youngClaim ) {  }
void NoteMajorWasMarked(const void* obj) {  }
void ReportYoungClaim(const char* point) {  }
uint8_t LookupMarkOrigin(uintptr_t obj, uint32_t* claimGc, uint8_t* majorSkip, uint32_t* majorGc, uint8_t* hasRef, uint8_t* nonYoungRef) { return 0; }
void NoteCrashWhoZero(uintptr_t r13, uintptr_t rcx, uintptr_t rsi, uintptr_t rbx, uintptr_t r12) {  }
void Report(const char* point) {  }
} // namespace HealPairDiag
} // namespace MapleRuntime
