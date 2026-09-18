# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "z/zHeap.cpp"
    "z/zInitialize.cpp"
    "z/zArguments.cpp"
    "z/zGlobals.cpp"
    "z/zThreadLocalData.cpp"
    "z/workerThread.cpp"
    "z/zTask.cpp"
    "z/zWorkers.cpp"
    "z/concurrentGCThread.cpp"
    "z/zThread.cpp"
    "z/zUtils.cpp"
    "z/zCPU.cpp"
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
if (WINDOWS_FLAG MATCHES 1)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/z/os/windows)
elseif (MACOS_FLAG MATCHES 1)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/z/os/bsd)
else()
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/z/os/linux)
endif()
add_subdirectory(Allocator)
add_subdirectory(Barrier)
add_subdirectory(Verify)

list(APPEND SRC_LIST
    "z/zBreakpoint.cpp"
    "z/concurrentGCBreakpoints.cpp"
    "z/zAbort.cpp"
    "z/zRelocateSupport.cpp"
    "z/zHeapIterator.cpp"
    "z/zIterator.cpp"
    "z/zCollectedHeap.cpp"
    "z/zDriver.cpp"
    "z/zDirector.cpp"
    "z/zDriverPort.cpp"
    "z/zTracing.cpp"
    "z/zRootsIterator.cpp"
    "z/zWeakRootsProcessor.cpp"
    "z/zResurrection.cpp"
    "z/zMarkStack.cpp"
    "z/zMarkingSMR.cpp"
    "z/zMarkCache.cpp"
    "z/zReferenceProcessor.cpp"
    "z/zStringDedup.cpp"
    "z/zUncommitter.cpp"
    "z/zForwarding.cpp"
)

list(APPEND SRC_LIST
    "z/zGenerationCycle.cpp"
    "z/zCrossVM.cpp"
    "z/zGeneration.cpp"
    "z/zMark.cpp"
    "z/zRelocate.cpp"
    "z/zJNICritical.cpp"
    "z/zRemembered.cpp"
    "z/zRelocationSet.cpp"
)

add_library(Heap STATIC ${SRC_LIST})
if (MRT_GC_UNIT_TESTS)
    target_compile_definitions(Heap PRIVATE MRT_TESTABLE_INTERNALS=1)
endif()
