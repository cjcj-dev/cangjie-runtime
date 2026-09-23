// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/z/zRuntimeWorkers.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Base/Log.h"

namespace MapleRuntime {
// zRuntimeWorkers.cpp:29-41: construct and activate the complete runtime pool.
ZRuntimeWorkers::ZRuntimeWorkers()
    : _workers("RuntimeWorker", ZHeuristics::nparallel_workers())
{
    _workers.initialize_workers();
    _workers.set_active_workers(_workers.max_workers());
    CHECK_DETAIL(_workers.active_workers() == _workers.max_workers(),
                 "Failed to create runtime workers");
}
}
