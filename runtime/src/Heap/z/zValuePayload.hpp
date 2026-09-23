#ifndef MRT_Z_VALUE_PAYLOAD_HPP
#define MRT_Z_VALUE_PAYLOAD_HPP

#include "Common/BaseObject.h"
#include <vector>

namespace MapleRuntime {
// Cangjie values can be headerless or stack-resident. Their payload describes
// storage separately from its oop map (ZGC oops/valuePayload.hpp).
class ValuePayload {
public:
    enum class Kind { Heap, Native, Uncolored };
    MAddress address;
    size_t size;
    Kind kind;
    std::vector<size_t> offsets;
    ValuePayload(MAddress address, size_t size);
    ValuePayload(MAddress address, size_t size, Kind kind);
    ValuePayload(MAddress address, size_t size, GCTib layout, Kind kind);
    ValuePayload(MAddress address, size_t size, BaseObject* layout, MAddress layoutStart);
    ValuePayload(MAddress address, size_t size, std::vector<size_t> offsets, Kind kind);
};
} // namespace MapleRuntime
#endif
