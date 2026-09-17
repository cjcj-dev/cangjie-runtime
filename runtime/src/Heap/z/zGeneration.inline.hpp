// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Base/Panic.h"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zMark.inline.hpp"

namespace MapleRuntime {
inline bool GenerationCycle::IsPhaseMark() const
{
    const auto value = Phase();
    return value == GC_PHASE_ENUM || value == GC_PHASE_TRACE || value == GC_PHASE_CLEAR_SATB_BUFFER;
}

// ZGeneration::mark_object / mark_object_if_active (zGeneration.inline.hpp:118-129).
template<bool resurrect, bool gcThread, bool follow, bool finalizable>
inline void GenerationCycle::MarkObject(zaddress address)
{
    ASSERT(IsPhaseMark());
    CHECK(mark != nullptr);
    mark->MarkObject<resurrect, gcThread, follow, finalizable>(address);
}

template<bool resurrect, bool gcThread, bool follow, bool finalizable>
inline void GenerationCycle::MarkObjectIfActive(zaddress address)
{
    if (IsPhaseMark()) {
        MarkObject<resurrect, gcThread, follow, finalizable>(address);
    }
}
}
