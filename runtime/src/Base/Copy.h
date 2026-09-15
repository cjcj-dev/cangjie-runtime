// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_BASE_COPY_H
#define MRT_BASE_COPY_H

#include <cassert>
#include <cstddef>
#include <cstdint>

#if defined(__aarch64__) && !defined(_WIN32)
extern "C" void MRT_CopyDisjointWords(const uintptr_t* from, uintptr_t* to, size_t count);
#endif

namespace MapleRuntime {
// The word-copy subset of utilities/copy.hpp. Cangjie uses uintptr_t words.
class Copy {
public:
    // utilities/copy.hpp:95-99
    static void disjoint_words_atomic(const uintptr_t* from, uintptr_t* to, size_t count)
    {
        assert_params_ok(from, to, sizeof(uintptr_t));
        assert_disjoint(from, to, count);
        pd_disjoint_words_atomic(from, to, count);
    }

protected:
    // utilities/copy.hpp:305-324. AtomicAccess load/store are relaxed accesses;
    // ordinary C++ assignments can be widened by the optimizer.
    static void shared_disjoint_words_atomic(const uintptr_t* from, uintptr_t* to, size_t count)
    {
        switch (count) {
            case 8: __atomic_store_n(&to[7], __atomic_load_n(&from[7], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
                // fall through
            case 7: __atomic_store_n(&to[6], __atomic_load_n(&from[6], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
                // fall through
            case 6: __atomic_store_n(&to[5], __atomic_load_n(&from[5], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
                // fall through
            case 5: __atomic_store_n(&to[4], __atomic_load_n(&from[4], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
                // fall through
            case 4: __atomic_store_n(&to[3], __atomic_load_n(&from[3], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
                // fall through
            case 3: __atomic_store_n(&to[2], __atomic_load_n(&from[2], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
                // fall through
            case 2: __atomic_store_n(&to[1], __atomic_load_n(&from[1], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
                // fall through
            case 1: __atomic_store_n(&to[0], __atomic_load_n(&from[0], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
                // fall through
            case 0: break;
            default:
                while (count-- > 0) {
                    __atomic_store_n(to++, __atomic_load_n(from++, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
                }
                break;
        }
    }

private:
    // utilities/copy.hpp:327-343. Integer distances also work for separate allocations.
    static bool params_disjoint(const uintptr_t* from, uintptr_t* to, size_t count)
    {
        const uintptr_t src = reinterpret_cast<uintptr_t>(from);
        const uintptr_t dst = reinterpret_cast<uintptr_t>(to);
        if (src < dst) {
            return (dst - src) / sizeof(uintptr_t) >= count;
        }
        return (src - dst) / sizeof(uintptr_t) >= count;
    }

    static void assert_disjoint(const uintptr_t* from, uintptr_t* to, size_t count)
    {
        assert(params_disjoint(from, to, count) && "source and destination overlap");
    }

    static void assert_params_ok(const void* from, void* to, size_t alignment)
    {
        assert(reinterpret_cast<uintptr_t>(from) % alignment == 0 && "source must be aligned");
        assert(reinterpret_cast<uintptr_t>(to) % alignment == 0 && "destination must be aligned");
    }

#if defined(__aarch64__) && !defined(_WIN32)
#include "Base/Copy_aarch64.inline.h"
#else
    // cpu/x86/copy_x86.hpp:75-77; windows_aarch64/copy_windows_aarch64.hpp:71-73
    static void pd_disjoint_words_atomic(const uintptr_t* from, uintptr_t* to, size_t count)
    {
        shared_disjoint_words_atomic(from, to, count);
    }
#endif
};
} // namespace MapleRuntime
#endif // MRT_BASE_COPY_H
