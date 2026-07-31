# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "Heap.cpp"
    "GcThreadPool.cpp"
    "StickyLog.cpp"
    "FixEdgeSet.cpp"
    "CanonicalWriteTable.cpp"
    "ForwardFactTable.cpp"
    "RelocationDiagnosticTable.cpp"
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
set_property(SOURCE CanonicalWriteTable.cpp APPEND PROPERTY COMPILE_OPTIONS -fvisibility=hidden)
add_subdirectory(Allocator)
add_subdirectory(Barrier)
add_subdirectory(Collector)
add_subdirectory(WCollector)
add_library(Heap STATIC ${SRC_LIST})
