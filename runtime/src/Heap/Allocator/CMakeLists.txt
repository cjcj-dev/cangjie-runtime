# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "../z/zLiveMap.cpp"
    "zPage.cpp"
    "zPageAllocator.cpp"

    "../z/zForwardingTable.cpp"
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
    "../z/zPageTable.cpp"
    "../z/zObjectAllocator.cpp"
    "../z/zObjArrayAllocator.cpp"
    "../z/zHeuristics.cpp"
    "../z/zRelocationSetSelector.cpp"
    "CartesianTree.cpp"
    "../z/zMappedCache.cpp"
    "../shared/collectedHeap.cpp"
)

# ZGC os/<os>/gc/z/: CMake selects the pd files. Do not list posix/linux
# unconditionally, and do not keep _WIN64/__APPLE__ in the shared files.
if (WINDOWS_FLAG MATCHES 1)
    list(APPEND SRC_LIST
        "../z/zVirtualMemoryManager_windows.cpp"
        "../z/zPhysicalMemoryBacking_windows.cpp"
        "../z/zMapper_windows.cpp"
        "../z/zSyscall_windows.cpp"
        "../z/zLargePages_windows.cpp"
        "../z/zNUMA_windows.cpp"
    )
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../z/os/windows)
elseif (MACOS_FLAG MATCHES 1)
    list(APPEND SRC_LIST
        "../z/zVirtualMemoryManager_posix.cpp"
        "../z/zPhysicalMemoryBacking_bsd.cpp"
        "../z/zLargePages_bsd.cpp"
        "../z/zNUMA_bsd.cpp"
    )
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../z/os/bsd)
else()
    list(APPEND SRC_LIST
        "../z/zVirtualMemoryManager_posix.cpp"
        "../z/zPhysicalMemoryBacking_linux.cpp"
        "../z/zMountPoint_linux.cpp"
        "../z/zLargePages_linux.cpp"
        "../z/zNUMA_linux.cpp"
    )
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../z/os/linux)
    add_compile_definitions(LINUX)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
add_library(Allocator STATIC ${SRC_LIST})
if (MRT_GC_UNIT_TESTS)
    target_compile_definitions(Allocator PRIVATE MRT_GC_UNIT_TESTS=1)
endif()
