# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "Allocator.cpp"
    "ForwardingTable.cpp"
    "MemMap.cpp"
    "RegionSpace.cpp"
    "RegionManager.cpp"
    "RouteDestHold.cpp"
    "CartesianTree.cpp"
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
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/z_index_distributor_unit.cpp)
    target_include_directories(cj_z_index_distributor_unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
    target_link_libraries(cj_z_index_distributor_unit PRIVATE cangjie-runtime pthread)
    add_test(NAME cj_z_index_distributor_unit COMMAND cj_z_index_distributor_unit)
endif()
