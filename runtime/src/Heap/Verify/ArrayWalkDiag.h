#ifndef MRT_ARRAY_WALK_DIAG_H
#define MRT_ARRAY_WALK_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;
class TypeInfo;

namespace ArrayWalkDiag {

bool Enabled();

void Begin(BaseObject* holder, uint64_t declared, TypeInfo* component, bool largeRegion);
void NoteVisit();
void NotePush();
void NoteSkipMarked();
void NoteSkipMarkedTarget(BaseObject* target);
void NoteSkipGate();
void NoteSkipNull();
void End();
void Report(const char* point);

} // namespace ArrayWalkDiag
} // namespace MapleRuntime

#endif
