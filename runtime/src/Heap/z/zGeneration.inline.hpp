// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Base/Panic.h"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zMark.inline.hpp"
#include "Heap/z/zRemembered.inline.hpp"
#include "Heap/z/zWorkers.inline.hpp"

namespace MapleRuntime {
// ZGC zGeneration.inline.hpp:86-88.
inline bool ZGeneration::should_worker_resize()
{
    return workers->should_worker_resize();
}

// ZGC zGeneration.inline.hpp:166-168.
inline bool ZGenerationYoung::is_remembered(volatile zpointer* p) const
{
    return remembered()->is_remembered(p);
}

inline bool ZGeneration::IsPhaseMark() const
{
    return is_phase_mark();
}

// ZGeneration::mark_object / mark_object_if_active (zGeneration.inline.hpp:118-129).
template<bool resurrect, bool gcThread, bool follow, bool finalizable>
inline void ZGeneration::MarkObject(zaddress address)
{
    ASSERT(IsPhaseMark());
    CHECK(mark != nullptr);
    mark->MarkObject<resurrect, gcThread, follow, finalizable>(address);
}

template<bool resurrect, bool gcThread, bool follow, bool finalizable>
inline void ZGeneration::MarkObjectIfActive(zaddress address)
{
    if (IsPhaseMark()) {
        MarkObject<resurrect, gcThread, follow, finalizable>(address);
    }
}
}
