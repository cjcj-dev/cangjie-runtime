#ifndef MRT_HOLE_WHO_DIAG_H
#define MRT_HOLE_WHO_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;
class RegionInfo;

namespace HoleWhoDiag {

enum class Bucket : unsigned {
    NEVER_WRITTEN = 0,
    AFTER_NEIGHBOR = 1,
    HEADER_CLEARED = 2,
    N = 3,
};

void NoteWalkBreak(RegionInfo* region, uintptr_t holeStart, uintptr_t allocPtr, BaseObject* prevObj,
                   size_t prevSize);
void Dump(const char* point);

} // namespace HoleWhoDiag
} // namespace MapleRuntime

#endif
