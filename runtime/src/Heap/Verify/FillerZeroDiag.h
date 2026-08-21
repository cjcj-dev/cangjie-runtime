#ifndef MRT_FILLER_ZERO_DIAG_H
#define MRT_FILLER_ZERO_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
namespace FillerZeroDiag {

enum class Site : unsigned {
    SLOT_EXTRA = 0,
    CLEAR_UNITS = 1,
    TAKE_GARBAGE = 2,
    TAKE_INACTIVE = 3,
    DIRTY_TAKE = 4,
    RELEASED_PRE = 5,
    COMPACT = 6,
    COMPACT_PARTIAL = 7,
    N = 8,
};

void Note(Site site, uintptr_t start, size_t size);
void Dump(const char* point);

} // namespace FillerZeroDiag
} // namespace MapleRuntime

#endif
