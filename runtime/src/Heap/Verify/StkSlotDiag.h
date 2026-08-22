#ifndef MRT_STK_SLOT_DIAG_H
#define MRT_STK_SLOT_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;

namespace StkSlotDiag {

constexpr bool kArmed = true;

void NoteAlloc(void* obj, size_t nElems);
void NoteZero(uintptr_t start, size_t size);
void NoteMapSlot(intptr_t bias, BaseObject* root);
void NoteMapReg(int reg, BaseObject* root);
void AfterFrame(uintptr_t startIP, uintptr_t frameIP, uintptr_t fa, bool mapValid,
                const char* funcName);
void Dump(const char* point);

} // namespace StkSlotDiag
} // namespace MapleRuntime

#endif
