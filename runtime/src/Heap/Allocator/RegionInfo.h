// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zPage.hpp"

namespace MapleRuntime {
inline unsigned RegionInfo::RelocateObserve() const
    {
        auto owner = ForwardingTable::RetainPageOwner(const_cast<RegionInfo*>(this));
        if (!owner) {
            return 0;
        }
        unsigned v = 1;
        if (owner->is_claimed()) {
            v |= 2;
        }
        if (owner->is_done()) {
            v |= 4;
        }
        if (owner->in_place()) {
            v |= 8;
        }
        return v;
    }

inline std::atomic<uint64_t>& RegionInfo::EnrolBeforeFlip()
    {
        static std::atomic<uint64_t> n{ 0 };
        return n;
    }

inline std::atomic<uint64_t>& RegionInfo::EnrolAfterFlip()
    {
        static std::atomic<uint64_t> n{ 0 };
        return n;
    }

}
