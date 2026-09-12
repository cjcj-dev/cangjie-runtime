// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_HEAP_WORK_H
#define MRT_HEAP_WORK_H

// Worker tasks are borrowed by synchronous worker dispatch. Kept as an
// include entry for collector components; the owning task queue is removed.
#include "Heap/GcThreadPool.h"

#endif // MRT_HEAP_WORK_H
