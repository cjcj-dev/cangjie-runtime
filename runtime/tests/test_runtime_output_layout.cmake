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

# Exercise the rejection predicate in a child script; the integration arm
# separately proves that Ninja Multi-Config sets the property passed here.
if(TEST_RUNTIME_OUTPUT_LAYOUT_MULTI_CONFIG_CHILD)
    cj_runtime_require_single_config_generator(TRUE)
    message(FATAL_ERROR "multi-config output layout was accepted")
endif()

# Run the typed cache-axis arms in child processes so INTERNAL is supplied by
# an actual -D command-line input.  Setting it here with CACHE INTERNAL would
# instead model project-generated CMake bookkeeping, which is intentionally
# excluded from product identity.
if(TEST_RUNTIME_OUTPUT_LAYOUT_CHILD)
    cj_runtime_configure_output_layout()
    message(STATUS
        "RUNTIME_OUTPUT_LAYOUT_CHILD id=${CANGJIE_RUNTIME_CONFIG_ID} root=${CMAKE_OUTPUT_DIRECTORY}")
    return()
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND}
        "-DTEST_RUNTIME_SOURCE_DIR=${TEST_RUNTIME_SOURCE_DIR}"
        -DTEST_RUNTIME_OUTPUT_LAYOUT_MULTI_CONFIG_CHILD=ON
        -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE _multi_config_rc
    OUTPUT_VARIABLE _multi_config_out
    ERROR_VARIABLE _multi_config_err)
if(_multi_config_rc EQUAL 0)
    message(FATAL_ERROR "multi-config output layout child unexpectedly succeeded")
endif()
if(NOT _multi_config_err MATCHES "supports only single-config generators" OR
        NOT _multi_config_err MATCHES "multi-config generators are not supported")
    message(FATAL_ERROR
        "multi-config output layout rejection text missing: ${_multi_config_err}")
endif()

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
# two directories and return to the original identity, independent of both a
# hand-maintained option list and the cache type selected by the caller.
foreach(_cache_type INTERNAL STATIC)
    string(TOLOWER "${_cache_type}" _cache_type_lower)
    set(_typed_arm_index 0)
    foreach(_value OFF ON OFF)
        math(EXPR _typed_arm_index "${_typed_arm_index}+1")
        execute_process(
            COMMAND ${CMAKE_COMMAND}
                "-DTEST_RUNTIME_SOURCE_DIR=${TEST_RUNTIME_SOURCE_DIR}"
                -DTEST_RUNTIME_OUTPUT_LAYOUT_CHILD:INTERNAL=ON
                "-DDISABLE_VERSION_CHECK:${_cache_type}=${_value}"
                -P "${CMAKE_CURRENT_LIST_FILE}"
            RESULT_VARIABLE _typed_arm_rc
            OUTPUT_VARIABLE _typed_arm_out
            ERROR_VARIABLE _typed_arm_err)
        if(NOT _typed_arm_rc EQUAL 0)
            message(FATAL_ERROR
                "${_cache_type} cache arm ${_typed_arm_index} failed: ${_typed_arm_err}")
        endif()
        if(NOT _typed_arm_out MATCHES
                "RUNTIME_OUTPUT_LAYOUT_CHILD id=([^ ]+) root=([^ \n]+)")
            message(FATAL_ERROR
                "${_cache_type} cache arm ${_typed_arm_index} produced no identity: ${_typed_arm_out}")
        endif()
        if(_typed_arm_index EQUAL 1)
            set(${_cache_type_lower}_off_id "${CMAKE_MATCH_1}")
            set(${_cache_type_lower}_off_root "${CMAKE_MATCH_2}")
        elseif(_typed_arm_index EQUAL 2)
            set(${_cache_type_lower}_on_id "${CMAKE_MATCH_1}")
            set(${_cache_type_lower}_on_root "${CMAKE_MATCH_2}")
        else()
            set(${_cache_type_lower}_restored_id "${CMAKE_MATCH_1}")
            set(${_cache_type_lower}_restored_root "${CMAKE_MATCH_2}")
        endif()
    endforeach()

    if(${_cache_type_lower}_off_id STREQUAL ${_cache_type_lower}_on_id)
        message(FATAL_ERROR
            "${_cache_type} DISABLE_VERSION_CHECK OFF and ON share an identity")
    endif()
    if(NOT ${_cache_type_lower}_off_id STREQUAL ${_cache_type_lower}_restored_id OR
            NOT ${_cache_type_lower}_off_root STREQUAL ${_cache_type_lower}_restored_root)
        message(FATAL_ERROR
            "restoring ${_cache_type} DISABLE_VERSION_CHECK did not restore output identity")
    endif()
    if(${_cache_type_lower}_off_root STREQUAL ${_cache_type_lower}_on_root)
        message(FATAL_ERROR
            "${_cache_type} DISABLE_VERSION_CHECK OFF and ON share an output directory")
    endif()

    foreach(pair
            "${${_cache_type_lower}_off_root};OFF"
            "${${_cache_type_lower}_on_root};ON")
        list(GET pair 0 output_root)
        list(GET pair 1 expected_value)
        file(READ "${output_root}/runtime-build-config.txt" manifest)
        if(NOT manifest MATCHES "DISABLE_VERSION_CHECK=${expected_value}[
]")
            message(FATAL_ERROR
                "${_cache_type} DISABLE_VERSION_CHECK ${expected_value} output has the wrong manifest")
        endif()
    endforeach()
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
    "RUNTIME_OUTPUT_LAYOUT_OK default=${default_id} testable=${testable_id} gcunit=${gcunit_id} internal=${internal_off_id}->${internal_on_id}->${internal_restored_id} static=${static_off_id}->${static_on_id}->${static_restored_id}")
