# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "z/zHeap.cpp"
    "z/zInitialize.cpp"
    "z/zGlobals.cpp"
    "z/zWorkers.cpp"
    "z/zRuntimeWorkers.cpp"
    "z/workerThread.cpp"
    "z/zTask.cpp"
    "z/zUtils.cpp"
    "z/zCPU.cpp"
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
# OS_HEADER(gc/z/zPhysicalMemoryBacking) stand-in: CMake picks the pd header.
if (CMAKE_SYSTEM_NAME MATCHES "Windows")
    set(Z_PMM_BACKING_HEADER "zPhysicalMemoryBacking_windows.hpp")
elseif (CMAKE_SYSTEM_NAME MATCHES "Darwin" OR CMAKE_SYSTEM_NAME MATCHES "iOS")
    set(Z_PMM_BACKING_HEADER "zPhysicalMemoryBacking_bsd.hpp")
else()
    set(Z_PMM_BACKING_HEADER "zPhysicalMemoryBacking_linux.hpp")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/zPhysicalMemoryBacking.hpp"
     "#include \"Heap/z/${Z_PMM_BACKING_HEADER}\"\n")
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR}/Allocator)
add_subdirectory(Allocator)
add_subdirectory(Barrier)
add_subdirectory(Collector)
add_subdirectory(WCollector)
add_subdirectory(Verify)
add_library(Heap STATIC ${SRC_LIST})
if (MRT_GC_UNIT_TESTS)
    target_compile_definitions(Heap PRIVATE MRT_TESTABLE_INTERNALS=1)
endif()
