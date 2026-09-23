// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#ifndef MRT_Z_REFERENCE_DISCOVERER_HPP
#define MRT_Z_REFERENCE_DISCOVERER_HPP

#include <cstdint>

namespace MapleRuntime {
class BaseObject;

enum class ReferenceType : uint8_t {
    SOFT = 0,
    WEAK,
    FINAL,
    PHANTOM,
    COUNT,
};

// HotSpot gc/shared/referenceDiscoverer.hpp: the closure carries this
// interface, while the collector owns the implementation and its lifetime.
class ReferenceDiscoverer {
public:
    virtual bool discover_reference(BaseObject* object, ReferenceType type) = 0;
};
} // namespace MapleRuntime
#endif
