cmake_minimum_required(VERSION 3.16)

set(CMAKE_CURRENT_SOURCE_DIR "${TEST_RUNTIME_SOURCE_DIR}")
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_FLAGS "")
set(CMAKE_CXX_FLAGS "")
set(CMAKE_SHARED_LINKER_FLAGS "")
set(GLOBAL_EXPORT_FLAG 1)
set(MRT_GC_UNIT_TESTS OFF)
set(MRT_TESTABLE_INTERNALS OFF)
include("${TEST_RUNTIME_SOURCE_DIR}/build/cmake/RuntimeOutputLayout.cmake")

cj_runtime_configure_output_layout()
set(default_id "${CANGJIE_RUNTIME_CONFIG_ID}")
set(default_root "${CMAKE_OUTPUT_DIRECTORY}")

set(MRT_TESTABLE_INTERNALS ON)
cj_runtime_configure_output_layout()
set(testable_id "${CANGJIE_RUNTIME_CONFIG_ID}")
set(testable_root "${CMAKE_OUTPUT_DIRECTORY}")

set(MRT_GC_UNIT_TESTS ON)
cj_runtime_configure_output_layout()
set(gcunit_id "${CANGJIE_RUNTIME_CONFIG_ID}")
set(gcunit_root "${CMAKE_OUTPUT_DIRECTORY}")

if(default_id STREQUAL testable_id OR default_id STREQUAL gcunit_id OR testable_id STREQUAL gcunit_id)
    message(FATAL_ERROR "runtime configurations share an identity")
endif()
foreach(pair
        "${default_id};${default_root};default"
        "${testable_id};${testable_root};testable"
        "${gcunit_id};${gcunit_root};gcunit")
    list(GET pair 0 config_id)
    list(GET pair 1 output_root)
    list(GET pair 2 profile)
    if(NOT output_root MATCHES "/output/temp/${config_id}$")
        message(FATAL_ERROR "${profile} output root does not contain its configuration id")
    endif()
    file(READ "${output_root}/runtime-build-config.txt" manifest)
    if(NOT manifest MATCHES "CONFIG_ID=${config_id}[
]" OR NOT manifest MATCHES "CONFIG_SIGNATURE_SHA256=[0-9a-f]+[
]")
        message(FATAL_ERROR "${profile} manifest does not bind its configuration")
    endif()
endforeach()

message(STATUS "RUNTIME_OUTPUT_LAYOUT_OK default=${default_id} testable=${testable_id} gcunit=${gcunit_id}")
