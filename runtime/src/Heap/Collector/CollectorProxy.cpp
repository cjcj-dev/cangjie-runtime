// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CollectorProxy.h"

namespace MapleRuntime {
void CollectorProxy::Init()
{
    wCollector.Init();

    if (currentCollector == nullptr) {
        currentCollector = &wCollector;
    }
}

void CollectorProxy::Fini()
{
    wCollector.Fini();
}

void CollectorProxy::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
    // The collector is bound at Init; concurrent drivers only select a
    // generation inside it (zGeneration.inline.hpp:66-71).
    currentCollector->RunGarbageCollection(gcIndex, reason);
}
} // namespace MapleRuntime
