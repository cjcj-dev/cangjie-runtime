// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#pragma once
#include <cstddef>
namespace MapleRuntime {
// recent-full is a lifecycle queue, not a liveness root. Account at ownership
// transitions so retained inventory can be separated from ordinary heap growth.
namespace RecentFullAccounting {
void Enqueue(size_t regions, size_t units);
void Dequeue(size_t regions, size_t units);
void Report(size_t listRegions, size_t listBytes);
}

}
