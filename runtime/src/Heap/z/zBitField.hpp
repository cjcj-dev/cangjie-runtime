// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_BITFIELD_HPP
#define MRT_Z_BITFIELD_HPP

#include <cstddef>
#include <cstdint>

#include "Base/Log.h"

namespace MapleRuntime {

//
// ZGC zBitField.hpp:29-79. Static encode/decode of one field inside a
// container word.
//
//  Example
//  -------
//
//  typedef ZBitField<uint64_t, uint8_t,  0,  2, 3> field_word_aligned_size;
//  typedef ZBitField<uint64_t, uint32_t, 2, 30>    field_length;
//
//  field_word_aligned_size::encode(16) = 2
//  field_length::encode(2342) = 9368
//
//  field_word_aligned_size::decode(9368 | 2) = 16
//  field_length::decode(9368 | 2) = 2342
//

template <typename ContainerType, typename ValueType, int FieldShift, int FieldBits, int ValueShift = 0>
class ZBitField {
private:
    static const int BitsPerByte = 8;
    static const int ContainerBits = sizeof(ContainerType) * BitsPerByte;

    static_assert(FieldBits < ContainerBits, "Field too large");
    static_assert(FieldShift + FieldBits <= ContainerBits, "Field too large");
    static_assert(ValueShift + FieldBits <= ContainerBits, "Field too large");

    static const ContainerType FieldMask = (((ContainerType)1 << FieldBits) - 1);

    ZBitField() = delete;

public:
    static ValueType decode(ContainerType container)
    {
        return (ValueType)(((container >> FieldShift) & FieldMask) << ValueShift);
    }

    static ContainerType encode(ValueType value)
    {
        DCHECK_D(((ContainerType)value & (FieldMask << ValueShift)) == (ContainerType)value, "Invalid value");
        return (ContainerType)(((ContainerType)value >> ValueShift) << FieldShift);
    }
};

} // namespace MapleRuntime

#endif // MRT_Z_BITFIELD_HPP
