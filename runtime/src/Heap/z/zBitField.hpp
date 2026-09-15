// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#pragma once
#include <cstddef>
#include <limits>
namespace MapleRuntime {
template<typename T>
class BitField {
public:
    // pos: the position where the bit locates. It starts from 0.
    // bitLen: the length that is to be read.
    T GetAtomicValue(size_t pos, size_t bitLen) const
    {
        T value = __atomic_load_n(&fieldVal, __ATOMIC_ACQUIRE);
        T bitMask = FieldMask(pos, bitLen);
        return value & bitMask;
    }
    void SetAtomicValue(size_t pos, size_t bitLen, T newValue)
    {
        do {
            T oldValue = fieldVal;
            T bitMask = FieldMask(pos, bitLen);
            T unchangedBitMask = ~bitMask;
            T newFieldValue = (static_cast<T>(newValue << pos) & bitMask) | (oldValue & unchangedBitMask);
            if (__atomic_compare_exchange_n(&fieldVal, &oldValue, newFieldValue, false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                return;
            }
        } while (true);
    }

private:
    static constexpr T FieldMask(size_t pos, size_t bitLen)
    {
        constexpr size_t width = std::numeric_limits<T>::digits;
        const T lowMask = bitLen >= width ? static_cast<T>(~T(0))
                                          : static_cast<T>((T(1) << bitLen) - T(1));
        return static_cast<T>(lowMask << pos);
    }

    T fieldVal;
};
}
