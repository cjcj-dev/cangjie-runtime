// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#pragma once
#include "Heap/z/zBitField.hpp"
namespace MapleRuntime {
// ZGC zAllocationFlags.hpp:55-92.
class ZAllocationFlags {
    using field_non_blocking = ZBitField<uint8_t, bool, 0, 1>;
    using field_gc_relocation = ZBitField<uint8_t, bool, 1, 1>;
    using field_fast_medium = ZBitField<uint8_t, bool, 2, 1>;
    uint8_t flags{0};
public:
    void set_non_blocking() { flags |= field_non_blocking::encode(true); }
    void set_gc_relocation() { flags |= field_gc_relocation::encode(true); }
    void set_fast_medium() { flags |= field_fast_medium::encode(true); }
    bool non_blocking() const { return field_non_blocking::decode(flags); }
    bool gc_relocation() const { return field_gc_relocation::decode(flags); }
    bool fast_medium() const { return field_fast_medium::decode(flags); }
};
} // namespace MapleRuntime
