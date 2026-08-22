#ifndef MRT_ARRAY_WALK_DIAG_H
#define MRT_ARRAY_WALK_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;
class TypeInfo;

namespace ArrayWalkDiag {

bool Enabled();
bool SlotWatchEnabled();
uint64_t SlotWatchIndex();

void Begin(BaseObject* holder, uint64_t declared, TypeInfo* component, bool largeRegion);
void NoteVisit();
void NotePush();
void NoteSkipMarked();
void NoteSkipMarkedTarget(BaseObject* target);
void NoteSkipGate();
void NoteSkipGateMarkGood();
void NoteSkipGatePlausible();
void NoteSkipNull();
void ReportSlotWatch(BaseObject* holder, uint64_t declared, uint64_t index, uintptr_t slotAddress,
                     uint64_t slotValue, BaseObject* target, bool walkVisited, bool pushed,
                     const char* skipReason, int isMarkedObject);
void ReportSlotWatchCycleEnd();
void End();
void Report(const char* point);

} // namespace ArrayWalkDiag
} // namespace MapleRuntime

#endif
