#ifndef MRT_LOST_WRITE_PROBE_H
#define MRT_LOST_WRITE_PROBE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;
class Collector;

namespace LostWriteProbe {

constexpr bool kEnabled = true;

inline bool Enabled() { return kEnabled; }

void NoteWrite(BaseObject* obj, const void* field, const Collector& collector);

void NoteResolveNull(BaseObject* from, bool movableGhost);

void NoteTryForwardNull(BaseObject* from, bool movableGhost);

void Report(const char* point);

} // namespace LostWriteProbe
} // namespace MapleRuntime

#endif
