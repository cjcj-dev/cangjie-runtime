#ifndef MRT_ENUM_PUSH_DIAG_H
#define MRT_ENUM_PUSH_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;

// enumpush: did GcPhaseEnum / VisitHeapReferencesOnStack push a given stack-map root?
// Gate: MRT_GCV2_ENUMPUSH=1 or MRT_GCV2_DIAG token enumpush. Default off.
// Default-off path is a single static bool load; no output, no counters.
namespace EnumPushDiag {

bool Enabled();

void NoteFrame(uintptr_t startIP, uintptr_t frameIP, uintptr_t fa, int managed, int valid,
               int nSlots, int nRegs, const char* funcName);

void NoteMapSlot(uintptr_t fa, intptr_t bias, BaseObject* obj);
void NoteMapReg(uintptr_t fa, int reg, BaseObject* obj);

void NotePush(BaseObject* obj, const char* site, const void* slot);
void NoteSkip(BaseObject* obj, const char* site, const char* reason);

void Report(const char* point);

} // namespace EnumPushDiag
} // namespace MapleRuntime

#endif
