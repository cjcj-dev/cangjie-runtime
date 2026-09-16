# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "../z/zLiveMap.cpp"
    "zPage.cpp"
    "zPageAllocator.cpp"

    "Allocator.cpp"
    "zForwardingTable.cpp"
    "../z/zForwardingAllocator.cpp"
    "../z/zVirtualMemoryManager.cpp"
    "../z/zAddressSpaceLimit.cpp"
    "../z/zNUMA.cpp"
    "../z/zPhysicalMemoryManager.cpp"
    "../z/zLargePages.cpp"
    "../z/zErrno.cpp"
    "RegionSpace.cpp"
    "../z/zThreadLocalAllocBuffer.cpp"
    "../z/zTLABUsage.cpp"
    "../z/zPageAllocator.cpp"
    "../z/zPage.cpp"
    "../z/zObjectAllocator.cpp"
    "../z/zRelocationSetSelector.cpp"
    "CartesianTree.cpp"
    "../z/zMappedCache.cpp"
    "HeapFiller.cpp"
)

# ZGC os/<os>/gc/z/: CMake selects the pd files. Do not list posix/linux
# unconditionally, and do not keep _WIN64/__APPLE__ in the shared files.
if (CMAKE_SYSTEM_NAME MATCHES "Windows")
    list(APPEND SRC_LIST
        "../z/zVirtualMemoryManager_windows.cpp"
        "../z/zPhysicalMemoryBacking_windows.cpp"
        "../z/zMapper_windows.cpp"
        "../z/zSyscall_windows.cpp"
        "../z/zLargePages_windows.cpp"
        "../z/zNUMA_windows.cpp"
    )
    set(Z_PMM_BACKING_HEADER "zPhysicalMemoryBacking_windows.hpp")
elseif (CMAKE_SYSTEM_NAME MATCHES "Darwin" OR CMAKE_SYSTEM_NAME MATCHES "iOS")
    list(APPEND SRC_LIST
        "../z/zVirtualMemoryManager_posix.cpp"
        "../z/zPhysicalMemoryBacking_bsd.cpp"
        "../z/zLargePages_bsd.cpp"
        "../z/zNUMA_bsd.cpp"
    )
    set(Z_PMM_BACKING_HEADER "zPhysicalMemoryBacking_bsd.hpp")
else()
    list(APPEND SRC_LIST
        "../z/zVirtualMemoryManager_posix.cpp"
        "../z/zPhysicalMemoryBacking_linux.cpp"
        "../z/zMountPoint_linux.cpp"
        "../z/zLargePages_linux.cpp"
        "../z/zNUMA_linux.cpp"
    )
    set(Z_PMM_BACKING_HEADER "zPhysicalMemoryBacking_linux.hpp")
    add_compile_definitions(LINUX)
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/zPhysicalMemoryBacking.hpp"
     "#include \"Heap/z/${Z_PMM_BACKING_HEADER}\"\n")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
add_library(Allocator STATIC ${SRC_LIST})
if (MRT_GC_UNIT_TESTS)
    target_compile_definitions(Allocator PRIVATE MRT_GC_UNIT_TESTS=1)
endif()
