# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# GC unit tests — default OFF via MRT_GC_UNIT_TESTS (see runtime/config.cmake).
# U3–U7 link product libcangjie-runtime (live heap fixture); U1/U2 stay header-only.

set(GC_UNIT_SOURCES
    gc_unit_main.cpp
    gc_unit_stubs.cpp
    test_colour_address.cpp
    test_route_info.cpp
    test_live_map.cpp
    test_object_gate.cpp
    test_remset.cpp
)

add_executable(cj_gc_unit ${GC_UNIT_SOURCES})
target_include_directories(cj_gc_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/Heap
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(cj_gc_unit PRIVATE cxx_std_17)
target_compile_options(cj_gc_unit PRIVATE -fno-rtti)
# Prefer in-tree runtime library when building as part of the product tree.
if (TARGET cangjie-runtime)
    target_link_libraries(cj_gc_unit PRIVATE cangjie-runtime)
elseif (DEFINED GCV2_RUNTIME_LIB_DIR)
    target_link_directories(cj_gc_unit PRIVATE ${GCV2_RUNTIME_LIB_DIR})
    target_link_libraries(cj_gc_unit PRIVATE cangjie-runtime boundscheck)
    set_target_properties(cj_gc_unit PROPERTIES
        BUILD_RPATH "${GCV2_RUNTIME_LIB_DIR}"
        INSTALL_RPATH "${GCV2_RUNTIME_LIB_DIR}")
endif()

enable_testing()
add_test(NAME cj_gc_unit COMMAND cj_gc_unit)
