// Read the product's actual TLAB words; never create or seed an allocator.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "Mutator/ThreadLocal.h"

namespace {
// Only the single managed fixture task calls these two functions. Keep its
// snapshot across a possible migration to another worker thread.
uintptr_t before[3];
const void* beforeBuffer;
void ReadBounds(uintptr_t (&words)[3])
{
    const auto* buffer = MapleRuntime::ThreadLocal::GetAllocBuffer();
    std::memset(words, 0, sizeof(words));
    if (buffer != nullptr) { std::memcpy(words, buffer, sizeof(words)); }
}
}
extern "C" void TLABBefore()
{
    beforeBuffer = MapleRuntime::ThreadLocal::GetAllocBuffer();
    ReadBounds(before);
}
extern "C" int64_t TLABAfter(int64_t expected, int64_t actual)
{
    uintptr_t after[3];
    ReadBounds(after);
    const bool bounds = after[2] != 0 && after[2] <= after[0] && after[0] <= after[1];
    const bool value = expected == actual;
    const bool sameBuffer = beforeBuffer == MapleRuntime::ThreadLocal::GetAllocBuffer();
    const bool refill = after[1] != before[1];
    const bool advance = !sameBuffer || refill || after[0] > before[0];
    std::fprintf(stderr, "TLAB_MANAGED_BOUNDS top=%#zx end=%#zx start=%#zx before_top=%#zx before_end=%#zx value=%d bounds=%d advance=%d refill=%d same_buffer=%d\n",
                 after[0], after[1], after[2], before[0], before[1], value, bounds, advance, refill, sameBuffer);
    if (!bounds || !value || !advance) { return -1; }
    // Migration changes the owner; it cannot certify either allocation branch.
    return !sameBuffer ? 3 : (refill ? 2 : 1);
}
