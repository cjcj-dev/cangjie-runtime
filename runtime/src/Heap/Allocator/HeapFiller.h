#ifndef MRT_HEAP_FILLER_H
#define MRT_HEAP_FILLER_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;

namespace HeapFiller {

bool Enabled();
void ZeroAndFill(uintptr_t start, size_t size);
bool IsFiller(const BaseObject* obj);

} // namespace HeapFiller
} // namespace MapleRuntime

#endif
