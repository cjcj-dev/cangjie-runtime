# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "../z/zBreakpoint.cpp"
    "../z/concurrentGCBreakpoints.cpp"
    "../z/zAbort.cpp"
    "../z/zMetronome.cpp"
    "zRelocate.cpp"

    "../z/zHeapIterator.cpp"
    "GcRequest.cpp"
    "GcStats.cpp"
    "../z/zCollectedHeap.cpp"
    "Collector.cpp"
    "CollectorProxy.cpp"
    "../z/zDriver.cpp"
    "../z/zDirector.cpp"
    "../z/zDriverPort.cpp"
    "TracingCollector.cpp"
    "../z/zRootsIterator.cpp"
    "../z/zMarkStack.cpp"
    "MarkStripe.cpp"
    "../z/zMarkingSMR.cpp"
    "../z/zMarkCache.cpp"
    "TaskQueue.cpp"
    "FinalizerProcessor.cpp"
    "../z/zReferenceProcessor.cpp"
    "StringDedup.cpp"
    "../z/zUncommitter.cpp"
    "CopyCollector.cpp"
    "LiveInfoArena.cpp"
    "../z/zForwarding.cpp"
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
add_library(Collector STATIC ${SRC_LIST})
if (MRT_GC_UNIT_TESTS)
    target_compile_definitions(Collector PRIVATE MRT_TESTABLE_INTERNALS=1)
endif()
