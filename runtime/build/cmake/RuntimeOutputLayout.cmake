# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.

# Compute a stable, human-readable output directory for one runtime build
# configuration.  The readable profile makes accidental cross-configuration
# use visible, while the digest distinguishes less common compiler/feature
# combinations without relying on a shared "latest" directory.
function(cj_runtime_configure_output_layout)
    # Every user-provided CMake cache input is part of the build identity.  A
    # hand-maintained option list is unsafe here: a newly introduced -D axis
    # can change the produced libraries while silently retaining an old output
    # directory.  INTERNAL and STATIC are legal -D types too; retain those
    # command-line inputs while excluding CMake's generated bookkeeping, whose
    # presence and binary-directory values change across equivalent configures.
    set(_signature "SCHEMA_VERSION=2\n")
    get_cmake_property(_cache_variables CACHE_VARIABLES)
    list(SORT _cache_variables)
    foreach(_variable IN LISTS _cache_variables)
        if(_variable MATCHES "^CANGJIE_RUNTIME_CONFIG_" OR
                _variable STREQUAL "OUTPUT_TEMP_PATH")
            continue()
        endif()
        get_property(_cache_type CACHE "${_variable}" PROPERTY TYPE)
        if(_cache_type STREQUAL "INTERNAL" OR _cache_type STREQUAL "STATIC")
            get_property(_cache_help CACHE "${_variable}" PROPERTY HELPSTRING)
            if(NOT _cache_help STREQUAL
                    "No help, variable specified on the command line.")
                continue()
            endif()
        endif()
        get_property(_cache_value CACHE "${_variable}" PROPERTY VALUE)
        string(APPEND _signature
            "CACHE:${_variable}:${_cache_type}=${_cache_value}\n")
    endforeach()

    # A few effective settings are ordinary variables after config.cmake has
    # normalized them.  Include those values as well as the cache inputs above.
    set(_identity_variables
        CMAKE_SYSTEM_NAME
        CMAKE_SYSTEM_PROCESSOR
        CMAKE_BUILD_TYPE
        CMAKE_C_COMPILER
        CMAKE_CXX_COMPILER
        CMAKE_C_FLAGS
        CMAKE_CXX_FLAGS
        CMAKE_SHARED_LINKER_FLAGS
        COPYGC_FLAG
        DOPRA_FLAG
        OHOS_FLAG
        ANDROID_FLAG
        IOS_FLAG
        IOS_SIMULATOR_FLAG
        MACOS_FLAG
        WINDOWS_FLAG
        EULER_FLAG
        RUNTIME_TRACE_FLAG
        DUMPADDRESS_FLAG
        COV
        ASAN_FLAG
        HWASAN_FLAG
        SANITIZER_SUPPORT
        GWPASAN_SUPPORT_FLAG
        GLOBAL_EXPORT_FLAG
        MRT_GCV2_UNTAG_BREADCRUMB
        MRT_ZSTAT
        MRT_GC_UNIT_TESTS
        MRT_TESTABLE_INTERNALS
        MRT_M0_CORRELATION_EXPERIMENT
        CANGJIE_RUNTIME_CONFIG_TAG)
    foreach(_variable IN LISTS _identity_variables)
        string(APPEND _signature "${_variable}=${${_variable}}\n")
    endforeach()
    string(SHA256 _signature_sha256 "${_signature}")
    string(SUBSTRING "${_signature_sha256}" 0 12 _signature_short)

    set(_profile "default")
    if(MRT_GC_UNIT_TESTS)
        set(_profile "gcunit")
    elseif(MRT_TESTABLE_INTERNALS)
        set(_profile "testable")
    endif()
    if(NOT GLOBAL_EXPORT_FLAG)
        string(APPEND _profile "-local-export")
    endif()
    if(MRT_ZSTAT)
        string(APPEND _profile "-zstat")
    endif()
    if(MRT_M0_CORRELATION_EXPERIMENT)
        string(APPEND _profile "-m0corr")
    endif()
    if(ASAN_FLAG OR SANITIZER_SUPPORT STREQUAL "asan")
        string(APPEND _profile "-asan")
    elseif(HWASAN_FLAG OR SANITIZER_SUPPORT STREQUAL "hwasan")
        string(APPEND _profile "-hwasan")
    elseif(SANITIZER_SUPPORT STREQUAL "tsan")
        string(APPEND _profile "-tsan")
    endif()

    set(_readable "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_BUILD_TYPE}-${_profile}")
    string(TOLOWER "${_readable}" _readable)
    string(REGEX REPLACE "[^a-z0-9._+-]+" "-" _readable "${_readable}")
    set(_config_id "${_readable}-${_signature_short}")
    set(_output_root "${CMAKE_CURRENT_SOURCE_DIR}/output/temp/${_config_id}")
    set(_library_dir "${_output_root}/lib/${CMAKE_SYSTEM_PROCESSOR}_${CMAKE_BUILD_TYPE}")

    file(MAKE_DIRECTORY "${_output_root}")
    file(WRITE "${_output_root}/runtime-build-config.txt"
        "SCHEMA_VERSION=2\n"
        "CONFIG_ID=${_config_id}\n"
        "CONFIG_SIGNATURE_SHA256=${_signature_sha256}\n"
        "LIB_DIR=${_library_dir}\n"
        "BUILD_TYPE=${CMAKE_BUILD_TYPE}\n"
        "SYSTEM_NAME=${CMAKE_SYSTEM_NAME}\n"
        "SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}\n"
        "MRT_GC_UNIT_TESTS=${MRT_GC_UNIT_TESTS}\n"
        "MRT_TESTABLE_INTERNALS=${MRT_TESTABLE_INTERNALS}\n"
        "GLOBAL_EXPORT_FLAG=${GLOBAL_EXPORT_FLAG}\n"
        "DISABLE_VERSION_CHECK=${DISABLE_VERSION_CHECK}\n")

    set(CANGJIE_RUNTIME_CONFIG_ID "${_config_id}" CACHE INTERNAL
        "Derived identity for the runtime output configuration" FORCE)
    set(CANGJIE_RUNTIME_CONFIG_SIGNATURE "${_signature_sha256}" CACHE INTERNAL
        "Full runtime output configuration signature" FORCE)
    set(CMAKE_OUTPUT_DIRECTORY "${_output_root}" PARENT_SCOPE)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${_library_dir}" PARENT_SCOPE)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY
        "${_output_root}/ar/${CMAKE_SYSTEM_PROCESSOR}_${CMAKE_BUILD_TYPE}" PARENT_SCOPE)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY
        "${_output_root}/bin/${CMAKE_SYSTEM_PROCESSOR}_${CMAKE_BUILD_TYPE}" PARENT_SCOPE)
    set(OUTPUT_TEMP_PATH "${_output_root}" CACHE FILEPATH
        "Configuration-specific runtime intermediate output root" FORCE)
    set(OUTPUT_TEMP_PATH "${_output_root}" PARENT_SCOPE)
    set(CANGJIE_RUNTIME_CONFIG_ID "${_config_id}" PARENT_SCOPE)
    set(CANGJIE_RUNTIME_CONFIG_SIGNATURE "${_signature_sha256}" PARENT_SCOPE)
endfunction()
