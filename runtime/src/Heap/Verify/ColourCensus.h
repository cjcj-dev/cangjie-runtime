// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_COLOUR_CENSUS_H
#define MRT_COLOUR_CENSUS_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

class BaseObject;

struct ColourCensusStats {
    size_t total = 0;
    size_t nulls = 0;
    size_t coloured = 0;
    size_t legacyPlain = 0;
    size_t illegal = 0;
    const void* firstPlainSlot = nullptr;
    uintptr_t firstPlainValue = 0;
    BaseObject* firstPlainHolder = nullptr;
    const void* firstIllegalSlot = nullptr;
    uintptr_t firstIllegalValue = 0;
    BaseObject* firstIllegalHolder = nullptr;

    void Observe(const void* slot, uintptr_t value, BaseObject* holder);
};

// Iterates only fields named by the object's GCTib.  The StateWord header is
// never passed to ClassifySlotWord (POINTER_COLOUR_CAMPAIGN R7).
void CensusObjectSlots(BaseObject* object, ColourCensusStats& stats);

// Safe-point heap walk, wired through the existing Objects verification face.
void VerifyColourCensus(const char* point);

#if defined(MRT_TESTABLE_INTERNALS)
// The production enforcement branch with an explicit armed value lets the GC
// death test avoid process-global getenv caching.  It is absent from default
// product builds; correctness does not depend on this seam.
void EnforceColourCensusForTesting(const ColourCensusStats& stats, bool armed);
#endif

} // namespace MapleRuntime

#endif // MRT_COLOUR_CENSUS_H
