# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.

# Compute a stable, human-readable output directory for one runtime build
# configuration.  The readable profile makes accidental cross-configuration
# use visible, while the digest distinguishes less common compiler/feature
# combinations without relying on a shared "latest" directory.
function(cj_runtime_require_single_config_generator is_multi_config)
    if(is_multi_config)
        message(FATAL_ERROR
            "Cangjie runtime output layout supports only single-config generators; multi-config generators are not supported")
    endif()
endfunction()

# Snapshot by phase, rather than by a list of recognized product option names.
# project(NONE) has loaded the toolchain; enable_language then introduces its
# compiler-probe temporaries. Their persisted results are hashed separately.
function(cj_runtime_capture_input_scope phase)
    get_cmake_property(_cj_scope_names VARIABLES)
    list(REMOVE_DUPLICATES _cj_scope_names)
    set_property(DIRECTORY PROPERTY "CJ_RUNTIME_SCOPE_${phase}" "${_cj_scope_names}")
    foreach(_cj_scope_name IN LISTS _cj_scope_names)
        string(SHA256 _cj_scope_hash "${${_cj_scope_name}}")
        set_property(DIRECTORY PROPERTY
            "CJ_RUNTIME_SCOPE_${phase}_${_cj_scope_name}" "${_cj_scope_hash}")
    endforeach()
endfunction()

function(cj_runtime_configure_output_layout)
    # Capture the caller's complete effective variable scope before introducing
    # function temporaries. Normal variables may shadow CACHE entries, including
    # arbitrary options introduced by toolchains or project includes.
    get_cmake_property(_effective_variables VARIABLES)
    get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    cj_runtime_require_single_config_generator("${_is_multi_config}")

    # Cache type and HELPSTRING do not identify origin: -C files, presets and
    # toolchains can supply INTERNAL/STATIC product options with arbitrary help.
    # Include every cache value except named outputs of CMake/runtime itself.
    # Do not exclude CMAKE_* as a namespace: compiler flags are product inputs.
    set(_generated_cache_variables
        CMAKE_CACHEFILE_DIR CMAKE_CACHE_MAJOR_VERSION CMAKE_CACHE_MINOR_VERSION
        CMAKE_CACHE_PATCH_VERSION CMAKE_NUMBER_OF_MAKEFILES
        CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT PRESET_CMAKE_SYSTEM_NAME
        REPOSITORY_PATH val
        SLOT_PROBE_CONTROL_OK SLOT_PROBE_WITNESS_OK MARK_GENERATION_CONTROL_OK
        C4_TABLE_CONTROL_OK one_ok)
    set(_signature "SCHEMA_VERSION=4\n")
    get_cmake_property(_cache_variables CACHE_VARIABLES)
    list(REMOVE_DUPLICATES _cache_variables)
    list(SORT _cache_variables)
    foreach(_variable IN LISTS _cache_variables)
        if(_variable STREQUAL "CANGJIE_RUNTIME_CONFIG_ID" OR
                _variable STREQUAL "CANGJIE_RUNTIME_CONFIG_SIGNATURE" OR
                _variable STREQUAL "OUTPUT_TEMP_PATH")
            continue()
        endif()
        get_property(_cache_type CACHE "${_variable}" PROPERTY TYPE)
        if((_cache_type STREQUAL "INTERNAL" OR _cache_type STREQUAL "STATIC") AND
                (_variable IN_LIST _generated_cache_variables OR
                 _variable MATCHES "-(ADVANCED|STRINGS)$"))
            continue()
        endif()
        # CMake normalizes compiler paths/types on first configure; a repeated
        # -D may leave the raw cache spelling short while the effective compiler
        # remains absolute. Identity follows the value consumed by the build.
        string(SHA256 _value_sha256 "${${_variable}}")
        string(APPEND _signature "CACHE:${_variable}=${_value_sha256}\n")
    endforeach()

    # Conservative closure of inputs to subsequent target construction. Do not
    # enumerate product option names here: a newly introduced ordinary variable
    # must participate without also editing this identity implementation.
    get_property(_input_variables DIRECTORY PROPERTY CJ_RUNTIME_SCOPE_INPUT)
    get_property(_detected_variables DIRECTORY PROPERTY CJ_RUNTIME_SCOPE_DETECTED)
    list(REMOVE_DUPLICATES _effective_variables)
    list(SORT _effective_variables)
    set(_probe_temporaries "")
    foreach(_variable IN LISTS _effective_variables)
        if(_variable STREQUAL "CANGJIE_RUNTIME_CONFIG_ID" OR
                _variable STREQUAL "CANGJIE_RUNTIME_CONFIG_SIGNATURE" OR
                _variable IN_LIST _generated_cache_variables OR
                _variable MATCHES "-(ADVANCED|STRINGS)$" OR
                _variable MATCHES "^CMAKE_MATCH_([0-9]+|COUNT)$" OR
                _variable MATCHES "^CMAKE_(OUTPUT|LIBRARY_OUTPUT|ARCHIVE_OUTPUT|RUNTIME_OUTPUT)_DIRECTORY$" OR
                _variable STREQUAL "OUTPUT_TEMP_PATH")
            continue()
        endif()
        # Hash values individually: semicolons/newlines in flags cannot forge
        # another variable record in the signature.
        string(SHA256 _value_sha256 "${${_variable}}")
        get_property(_detected_hash DIRECTORY PROPERTY
            "CJ_RUNTIME_SCOPE_DETECTED_${_variable}")
        if(_detected_variables AND NOT _variable IN_LIST _input_variables AND
                NOT _variable IN_LIST _cache_variables AND
                _value_sha256 STREQUAL _detected_hash)
            # Unchanged state introduced solely by compiler detection. Keep an
            # audit of this mechanically derived set, not an exclusion list.
            string(APPEND _probe_temporaries "${_variable}\n")
            continue()
        endif()
        string(APPEND _signature "EFFECTIVE:${_variable}=${_value_sha256}\n")
    endforeach()
    # Toolchains may add options/definitions directly instead of storing them in
    # variables. These directory properties are inherited by product targets.
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        foreach(_property COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES
                LINK_OPTIONS LINK_DIRECTORIES)
            get_directory_property(_value "${_property}")
            string(SHA256 _value_sha256 "${_value}")
            string(APPEND _signature "DIRECTORY:${_property}=${_value_sha256}\n")
        endforeach()
    endif()
    get_property(_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
    foreach(_language IN LISTS _languages)
        set(_compiler_state "${CMAKE_PLATFORM_INFO_DIR}/CMake${_language}Compiler.cmake")
        if(EXISTS "${_compiler_state}")
            file(SHA256 "${_compiler_state}" _compiler_sha256)
            string(APPEND _signature "COMPILER:${_language}=${_compiler_sha256}\n")
        endif()
    endforeach()
    if(CMAKE_TOOLCHAIN_FILE)
        get_filename_component(_toolchain "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE
            BASE_DIR "${CMAKE_BINARY_DIR}")
        if(NOT EXISTS "${_toolchain}")
            get_filename_component(_toolchain "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE
                BASE_DIR "${CMAKE_SOURCE_DIR}")
        endif()
        file(SHA256 "${_toolchain}" _toolchain_sha256)
        string(APPEND _signature "TOOLCHAIN:${_toolchain}=${_toolchain_sha256}\n")
    endif()
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
    file(WRITE "${_output_root}/runtime-build-inputs.txt" "${_signature}")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/runtime-compiler-probe-temporaries.txt"
            "${_probe_temporaries}")
    endif()
    file(WRITE "${_output_root}/runtime-build-config.txt"
        "SCHEMA_VERSION=4\n"
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
