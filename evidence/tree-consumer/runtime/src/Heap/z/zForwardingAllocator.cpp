#include "Heap/z/zForwardingAllocator.hpp"

namespace MapleRuntime {

ZForwardingAllocator::ZForwardingAllocator() : _start(nullptr), _end(nullptr), _top(nullptr) {}

ZForwardingAllocator::~ZForwardingAllocator() { std::free(_start); }

void ZForwardingAllocator::reset(size_t size)
{
    _start = static_cast<char*>(std::realloc(_start, size));
    _top.store(_start, std::memory_order_relaxed);
    _end = _start + size;
}

} // namespace MapleRuntime
