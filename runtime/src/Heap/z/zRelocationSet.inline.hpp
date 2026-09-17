// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_RELOCATION_SET_INLINE_H
#define MRT_RELOCATION_SET_INLINE_H

#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zGeneration.hpp"

namespace MapleRuntime {
    template<Generation G>
inline void RegionManager::PrepareFromRegionList()
    {
        Heap::GetHeap().GetCollector().GetZGeneration(
            G == Generation::Young ? ZGenerationId::young : ZGenerationId::old)
            .relocation_set().install_from_regions(fromRegionList);
    }

} // namespace MapleRuntime
#endif
