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

# Regression for cangjie-runtime#42: DISABLE_VERSION_CHECK is a supported
# CMake cache axis which changes product code.  OFF -> ON -> OFF must select
# two directories and return to the original identity, independent of any
# hand-maintained option list.
set(MRT_GC_UNIT_TESTS OFF)
set(MRT_TESTABLE_INTERNALS OFF)
set(DISABLE_VERSION_CHECK OFF CACHE BOOL "" FORCE)
cj_runtime_configure_output_layout()
set(disable_version_check_off_id "${CANGJIE_RUNTIME_CONFIG_ID}")
set(disable_version_check_off_root "${CMAKE_OUTPUT_DIRECTORY}")

set(DISABLE_VERSION_CHECK ON CACHE BOOL "" FORCE)
cj_runtime_configure_output_layout()
set(disable_version_check_on_id "${CANGJIE_RUNTIME_CONFIG_ID}")
set(disable_version_check_on_root "${CMAKE_OUTPUT_DIRECTORY}")

set(DISABLE_VERSION_CHECK OFF CACHE BOOL "" FORCE)
cj_runtime_configure_output_layout()
set(disable_version_check_restored_id "${CANGJIE_RUNTIME_CONFIG_ID}")
set(disable_version_check_restored_root "${CMAKE_OUTPUT_DIRECTORY}")

if(disable_version_check_off_id STREQUAL disable_version_check_on_id)
    message(FATAL_ERROR "DISABLE_VERSION_CHECK OFF and ON share an identity")
endif()
if(NOT disable_version_check_off_id STREQUAL disable_version_check_restored_id OR
        NOT disable_version_check_off_root STREQUAL disable_version_check_restored_root)
    message(FATAL_ERROR "restoring DISABLE_VERSION_CHECK did not restore the output identity")
endif()
if(disable_version_check_off_root STREQUAL disable_version_check_on_root)
    message(FATAL_ERROR "DISABLE_VERSION_CHECK OFF and ON share an output directory")
endif()

foreach(pair
        "${disable_version_check_off_root};OFF"
        "${disable_version_check_on_root};ON")
    list(GET pair 0 output_root)
    list(GET pair 1 expected_value)
    file(READ "${output_root}/runtime-build-config.txt" manifest)
    if(NOT manifest MATCHES "DISABLE_VERSION_CHECK=${expected_value}[
]")
        message(FATAL_ERROR
            "DISABLE_VERSION_CHECK ${expected_value} output has the wrong manifest")
    endif()
endforeach()

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

message(STATUS
    "RUNTIME_OUTPUT_LAYOUT_OK default=${default_id} testable=${testable_id} gcunit=${gcunit_id} disable_version_check_off=${disable_version_check_off_id} disable_version_check_on=${disable_version_check_on_id} restored=${disable_version_check_restored_id}")
