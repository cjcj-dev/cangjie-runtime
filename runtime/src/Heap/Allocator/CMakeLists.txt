# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "Allocator.cpp"
    "zForwardingTable.cpp"
    "../z/zForwardingAllocator.cpp"
    "../z/zVirtualMemoryManager.cpp"
    "../z/zAddressSpaceLimit.cpp"
    "../z/zNUMA.cpp"
    "../z/zPhysicalMemoryManager.cpp"
    "RegionSpace.cpp"
    "../z/zThreadLocalAllocBuffer.cpp"
    "../z/zTLABUsage.cpp"
    "../z/zPageAllocator.cpp"
    "../z/zPage.cpp"
    "../z/zObjectAllocator.cpp"
    "../z/zRelocationSetSelector.cpp"
    "RouteDestHold.cpp"
    "CartesianTree.cpp"
    "../z/zMappedCache.cpp"
    "HeapFiller.cpp"
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
add_library(Allocator STATIC ${SRC_LIST})
if (MRT_GC_UNIT_TESTS)
    target_compile_definitions(Allocator PRIVATE MRT_GC_UNIT_TESTS=1)
endif()

if (MRT_GC_UNIT_TESTS)
    # Product claim-tree and page-table templates; use the runtime page size.
    add_executable(cj_z_index_distributor_unit
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/test_zIndexDistributor.cpp)
    target_include_directories(cj_z_index_distributor_unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
    target_link_libraries(cj_z_index_distributor_unit PRIVATE cangjie-runtime pthread)
    add_test(NAME cj_z_index_distributor_unit COMMAND cj_z_index_distributor_unit)
endif()
