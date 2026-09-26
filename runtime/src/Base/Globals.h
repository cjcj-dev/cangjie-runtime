// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_GLOBALS_H
#define MRT_GLOBALS_H

#include <cstddef>
#include <limits>
#include <type_traits>

#include "Base/Log.h"

namespace MapleRuntime {
// Time Factors
constexpr uint64_t TIME_FACTOR = 1000LL;
constexpr uint64_t SECOND_TO_MILLI_SECOND = TIME_FACTOR;
constexpr uint64_t SECOND_TO_MICRO_SECOND = TIME_FACTOR * TIME_FACTOR;
constexpr uint64_t SECOND_TO_NANO_SECOND = TIME_FACTOR * TIME_FACTOR * TIME_FACTOR;
constexpr uint64_t MILLI_SECOND_TO_MICRO_SECOND = TIME_FACTOR;
constexpr uint64_t MICRO_SECOND_TO_NANO_SECOND = TIME_FACTOR;
constexpr uint64_t MILLI_SECOND_TO_NANO_SECOND = TIME_FACTOR * TIME_FACTOR;

constexpr size_t KB = 1024;
constexpr size_t MB = KB * KB;
constexpr size_t GB = KB * KB * KB;

// System default page size in linux
extern const size_t MRT_PAGE_SIZE;

// The Array Struct size
constexpr size_t ARRAY_STRUCT_SIZE = 3;

template<typename T>
struct Identity {
    using type = T;
};

// For rounding integers.
// Note: Omit the `n` from T type deduction, deduce only from the `x` argument.
template<typename T>
constexpr bool IsPowerOfTwo(T x)
{
    static_assert(std::is_integral<T>::value, "T must be integral");
    bool ret = false;
    if (x != 0) {
        ret = (x & (x - 1)) == 0;
    }
    return ret;
}

// utilities/powerOfTwo.hpp log2i_exact: exact log2 of a power of two.
template<typename T>
inline int Log2Exact(T value)
{
    static_assert(std::is_integral<T>::value, "T must be integral");
    DCHECK(IsPowerOfTwo(value));
    int result = 0;
    while ((static_cast<T>(1) << result) != value) {
        ++result;
    }
    return result;
}

// utilities/powerOfTwo.hpp round_up_power_of_2: the closest power of two
// greater than or equal to value. precondition: value > 0.
template<typename T>
inline T RoundUpPowerOfTwo(T value)
{
    static_assert(std::is_integral<T>::value, "T must be integral");
    DCHECK(value > 0);
    if (IsPowerOfTwo(value)) {
        return value;
    }
    T result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

// utilities/powerOfTwo.hpp next_power_of_2: the next power of two greater
// than value. precondition: value >= 0.
template<typename T>
inline T NextPowerOfTwo(T value)
{
    static_assert(std::is_integral<T>::value, "T must be integral");
    return RoundUpPowerOfTwo(static_cast<T>(value + 1));
}

template<typename T>
T RoundDown(T x, typename Identity<T>::type n)
{
    DCHECK(IsPowerOfTwo(n));
    return (x & -n);
}

template<typename T>
constexpr T RoundUp(T x, typename std::remove_reference<T>::type n)
{
    return RoundDown(x + n - 1, n);
}

// ZGC utilities/align.hpp:35-108: deduce the value and alignment separately.
template<typename T, std::enable_if_t<std::is_integral<T>::value, int> = 0>
constexpr T AlignmentMask(T alignment)
{
    DCHECK(IsPowerOfTwo(alignment));
    return alignment - 1;
}

template<typename T, std::enable_if_t<std::is_enum<T>::value, int> = 0>
constexpr auto AlignmentMask(T alignment)
{
    return AlignmentMask(static_cast<std::underlying_type_t<T>>(alignment));
}

template<typename T, typename A, std::enable_if_t<std::is_integral<T>::value, int> = 0>
constexpr bool IsAligned(T size, A alignment)
{
    return (size & AlignmentMask(alignment)) == 0;
}

template<typename T, typename A, std::enable_if_t<std::is_integral<T>::value, int> = 0>
constexpr T AlignDown(T size, A alignment)
{
    // Convert before complementing so a narrow alignment preserves high bits.
    T result = static_cast<T>(size & ~static_cast<T>(AlignmentMask(alignment)));
    DCHECK(IsAligned(result, alignment));
    return result;
}

template<typename T, typename A, std::enable_if_t<std::is_integral<T>::value, int> = 0>
constexpr bool CanAlignUp(T size, A alignment)
{
    return AlignDown(std::numeric_limits<T>::max(), alignment) >= size;
}

// ZGC utilities/checkedCast.hpp:38-43.
template<typename T, typename U>
constexpr T CheckedCast(U value)
{
    T result = static_cast<T>(value);
    DCHECK(static_cast<U>(result) == value);
    return result;
}

template<typename T, typename A, std::enable_if_t<std::is_integral<T>::value, int> = 0>
constexpr T AlignUp(T size, A alignment)
{
    DCHECK(CanAlignUp(size, alignment));
    T adjusted = CheckedCast<T>(size + AlignmentMask(alignment));
    return AlignDown(adjusted, alignment);
}

template<typename T, typename A>
inline T* AlignUp(T* ptr, A alignment)
{
    return reinterpret_cast<T*>(AlignUp(reinterpret_cast<uintptr_t>(ptr), alignment));
}
} // namespace MapleRuntime

#endif // MRT_GLOBALS_H
