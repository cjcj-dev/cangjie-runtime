# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.

function(cj_runtime_require_single_config_generator is_multi_config)
    if(is_multi_config)
        message(FATAL_ERROR
            "Cangjie runtime output layout supports only single-config generators; multi-config generators are not supported")
    endif()
endfunction()

# Configure needs CJThread headers before runtime targets exist. Keep every
# unpublished product private to the build tree; no shared output/temp fallback.
macro(cj_runtime_prepare_output_layout)
    set(CJ_RUNTIME_STAGING_ROOT "${CMAKE_BINARY_DIR}/runtime-staging")
    set(CMAKE_OUTPUT_DIRECTORY "${CJ_RUNTIME_STAGING_ROOT}")
    set(OUTPUT_TEMP_PATH "${CJ_RUNTIME_STAGING_ROOT}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CJ_RUNTIME_STAGING_ROOT}/lib/${CMAKE_SYSTEM_PROCESSOR}_${CMAKE_BUILD_TYPE}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CJ_RUNTIME_STAGING_ROOT}/ar/${CMAKE_SYSTEM_PROCESSOR}_${CMAKE_BUILD_TYPE}")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CJ_RUNTIME_STAGING_ROOT}/bin/${CMAKE_SYSTEM_PROCESSOR}_${CMAKE_BUILD_TYPE}")
    file(MAKE_DIRECTORY "${CJ_RUNTIME_STAGING_ROOT}")
endmacro()

function(cj_runtime_export_directory directory)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_target IN LISTS _targets)
        get_target_property(_type "${_target}" TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
            continue()
        endif()
        if(NOT _type STREQUAL "OBJECT_LIBRARY")
            get_target_property(_binary "${_target}" BINARY_DIR)
            set_property(GLOBAL APPEND PROPERTY CJ_RUNTIME_LINK_COMMAND_FILES
                "${_binary}/CMakeFiles/${_target}.dir/link.txt")
        endif()
        # These are generation-time compiler/linker inputs, not snapshots of
        # CMake variables or an allowlist of arbitrary target properties.
        # TARGET_GENEX_EVAL also resolves indirect custom TARGET_PROPERTY reads.
        file(GENERATE
            OUTPUT "${CMAKE_BINARY_DIR}/runtime-generated-inputs/${_target}-$<COMPILE_LANGUAGE>.txt"
            CONTENT "definitions=$<TARGET_GENEX_EVAL:${_target},$<TARGET_PROPERTY:${_target},COMPILE_DEFINITIONS>>\noptions=$<TARGET_GENEX_EVAL:${_target},$<TARGET_PROPERTY:${_target},COMPILE_OPTIONS>>\nincludes=$<TARGET_GENEX_EVAL:${_target},$<TARGET_PROPERTY:${_target},INCLUDE_DIRECTORIES>>\nlink_options=$<TARGET_GENEX_EVAL:${_target},$<TARGET_PROPERTY:${_target},LINK_OPTIONS>>\nlink_libraries=$<TARGET_GENEX_EVAL:${_target},$<TARGET_PROPERTY:${_target},LINK_LIBRARIES>>\n"
            TARGET "${_target}")
    endforeach()
    foreach(_child IN LISTS _children)
        cj_runtime_export_directory("${_child}")
    endforeach()
endfunction()

function(cj_runtime_finalize_output_layout)
    if(NOT TARGET cangjie-runtime OR NOT TARGET boundscheck)
        return()
    endif()
    # Do not let files for targets removed by reconfiguration enter identity.
    file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/runtime-generated-inputs")
    set_property(GLOBAL PROPERTY CJ_RUNTIME_LINK_COMMAND_FILES "")
    cj_runtime_export_directory("${CMAKE_SOURCE_DIR}")
    get_property(_link_files GLOBAL PROPERTY CJ_RUNTIME_LINK_COMMAND_FILES)
    string(REPLACE ";" "\n" _link_lines "${_link_files}")
    file(WRITE "${CMAKE_BINARY_DIR}/runtime-link-inputs.txt" "${_link_lines}\n")
    set(_profile default)
    if(MRT_GC_UNIT_TESTS)
        set(_profile gcunit)
    elseif(MRT_TESTABLE_INTERNALS)
        set(_profile testable)
    endif()
    set(_label "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_BUILD_TYPE}-${_profile}")
    string(TOLOWER "${_label}" _label)
    string(REGEX REPLACE "[^a-z0-9._+-]+" "-" _label "${_label}")
    # Remember the configure-produced CJThread inputs before runtime compilation
    # populates staging. The publisher hashes their bytes at publication time.
    file(GLOB_RECURSE _cjthread_inputs LIST_DIRECTORIES false "${CJ_RUNTIME_STAGING_ROOT}/include/*")
    file(GLOB _cjthread_archives "${CJ_RUNTIME_STAGING_ROOT}/lib/*.a")
    list(APPEND _cjthread_inputs ${_cjthread_archives})
    list(SORT _cjthread_inputs)
    string(REPLACE ";" "\n" _cjthread_input_lines "${_cjthread_inputs}")
    file(WRITE "${CMAKE_BINARY_DIR}/runtime-cjthread-inputs.txt" "${_cjthread_input_lines}\n")
    set(_gate_args "")
    if(NOT CMAKE_CROSSCOMPILING AND NOT WINDOWS_FLAG MATCHES 1 AND NOT MACOS_FLAG MATCHES 1
        AND NOT IOS_FLAG MATCHES 1 AND NOT IOS_SIMULATOR_FLAG MATCHES 1 AND NOT IOS_SIMULATOR_FLAG MATCHES 2)
        set(_gate_args --gate "${CMAKE_SOURCE_DIR}/tests/gc_unit/gate_gc_unit.sh")
    endif()
    set(_publish_args
        --source "${CMAKE_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}"
        --staging "${CJ_RUNTIME_STAGING_ROOT}" --label "${_label}"
        --library-subdir "${CMAKE_SYSTEM_PROCESSOR}_${CMAKE_BUILD_TYPE}"
        --runtime "$<TARGET_FILE:cangjie-runtime>" --boundscheck "$<TARGET_FILE:boundscheck>"
        --compiler-state "${CMAKE_PLATFORM_INFO_DIR}"
        --generator "${CMAKE_GENERATOR}" --make-program "${CMAKE_MAKE_PROGRAM}"
        --testable "$<BOOL:${MRT_TESTABLE_INTERNALS}>" --ohos "$<BOOL:${MRT_GC_UNIT_OHOS_HOST}>"
        ${_gate_args})
    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND _publish_args --toolchain "${CMAKE_TOOLCHAIN_FILE}")
    endif()
    # Real product entry: publish the linked pair before the gate can consume it.
    # boundscheck is already a link dependency; explicitly retain that ordering.
    add_dependencies(cangjie-runtime boundscheck)
    set(_args_file "${CMAKE_BINARY_DIR}/runtime-publish-args.txt")
    string(REPLACE ";" "\n" _args_lines "${_publish_args}")
    file(GENERATE OUTPUT "${_args_file}" CONTENT "${_args_lines}\n")
    set_target_properties(cangjie-runtime PROPERTIES CJ_RUNTIME_PUBLISH_ARGS "${_args_file}")
    # Also supports a deliberate repeat publication when the link is up to date.
    add_custom_target(publish_runtime_output
        COMMAND python3 "${CMAKE_SOURCE_DIR}/build/publish_runtime_output.py" "@${_args_file}"
        DEPENDS cangjie-runtime boundscheck VERBATIM)
endfunction()
