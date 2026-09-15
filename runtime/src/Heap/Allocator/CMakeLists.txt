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
    "../z/zVirtualMemoryManager_posix.cpp"
    "../z/zAddressSpaceLimit.cpp"
    "../z/zNUMA.cpp"
    "../z/zPhysicalMemoryManager.cpp"
    "../z/zPhysicalMemoryBacking_linux.cpp"
    "../z/zMountPoint_linux.cpp"
    "../z/zLargePages.cpp"
    "../z/zLargePages_linux.cpp"
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

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
add_library(Allocator STATIC ${SRC_LIST})
if (MRT_GC_UNIT_TESTS)
    target_compile_definitions(Allocator PRIVATE MRT_GC_UNIT_TESTS=1)
endif()
