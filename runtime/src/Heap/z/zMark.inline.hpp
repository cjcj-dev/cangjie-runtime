// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zMark.hpp"

namespace MapleRuntime {
bool TracingCollector::MarkObject(BaseObject* obj) const
    {
        RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
        // livesame: MarkObject adds live only on 0→1 (ZGC inc_live).
        bool marked = regionInfo->MarkObjectByOwner(obj);
        if (!marked) {
            size_t objSize = obj->GetSize();
            if (!fixReferences && regionInfo->IsFromRegion()) {
                DLOG(TRACE, "marking tag w-obj %p<cls %p>+%zu", obj, obj->GetTypeInfo(), objSize);
            }
        }
        return marked;
    }
}
