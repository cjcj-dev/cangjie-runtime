# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# GC unit tests — default OFF via MRT_GC_UNIT_TESTS (see runtime/config.cmake).
# Links only pure GC invariant TUs; does not start the full runtime.

set(GC_UNIT_SOURCES
    gc_unit_main.cpp
    test_colour_address.cpp
    test_route_info.cpp
    test_live_map.cpp
    test_object_gate.cpp
)

add_executable(cj_gc_unit ${GC_UNIT_SOURCES})
target_include_directories(cj_gc_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(cj_gc_unit PRIVATE cxx_std_17)
# Header-only colour types need no runtime .o; keep link surface empty on purpose.

enable_testing()
add_test(NAME cj_gc_unit COMMAND cj_gc_unit)
