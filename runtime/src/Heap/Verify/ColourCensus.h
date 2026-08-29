// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_COLOUR_CENSUS_H
#define MRT_COLOUR_CENSUS_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

#if defined(MRT_TESTABLE_INTERNALS)
#define MRT_PTRCOLOUR_TEST_API __attribute__((visibility("default")))
#else
#define MRT_PTRCOLOUR_TEST_API __attribute__((visibility("hidden")))
#endif

class BaseObject;

struct ColourCensusStats {
    size_t slots = 0;
    // Non-null slots.  The full-colour gate is coloured == total.
    size_t total = 0;
    size_t nulls = 0;
    size_t coloured = 0;
    size_t plain = 0;
    size_t illegal = 0;
    const void* firstPlainSlot = nullptr;
    uintptr_t firstPlainValue = 0;
    BaseObject* firstPlainHolder = nullptr;
    const void* firstIllegalSlot = nullptr;
    uintptr_t firstIllegalValue = 0;
    BaseObject* firstIllegalHolder = nullptr;

    MRT_PTRCOLOUR_TEST_API void Observe(const void* slot, uintptr_t value, BaseObject* holder);
};

// Iterates only fields named by the object's GCTib.  The StateWord header is
// never passed to ClassifySlotWord (POINTER_COLOUR_CAMPAIGN R7).
MRT_PTRCOLOUR_TEST_API void CensusObjectSlots(BaseObject* object, ColourCensusStats& stats);

// Safe-point heap walk, wired through the existing Objects verification face.
__attribute__((visibility("hidden"))) void VerifyColourCensus(const char* point);

#if defined(MRT_TESTABLE_INTERNALS)
// Test-only entry to the same unconditional production enforcement branch.
MRT_PTRCOLOUR_TEST_API void EnforceColourCensusForTesting(const ColourCensusStats& stats);
#endif

#undef MRT_PTRCOLOUR_TEST_API

} // namespace MapleRuntime

#endif // MRT_COLOUR_CENSUS_H
