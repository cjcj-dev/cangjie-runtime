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
# product private to the build tree; independent of the source tree.
macro(cj_runtime_prepare_output_layout)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    # Drop retired publisher cache entries when reusing a normal build tree.
    unset(CANGJIE_RUNTIME_CONFIG_ID CACHE)
    unset(CANGJIE_RUNTIME_CONFIG_SIGNATURE CACHE)
    unset(OUTPUT_TEMP_PATH CACHE)
    set(CJ_RUNTIME_STAGING_ROOT "${CMAKE_BINARY_DIR}/runtime-staging")
    set(CMAKE_OUTPUT_DIRECTORY "${CJ_RUNTIME_STAGING_ROOT}")
    set(OUTPUT_TEMP_PATH "${CJ_RUNTIME_STAGING_ROOT}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CJ_RUNTIME_STAGING_ROOT}/lib/${CMAKE_SYSTEM_PROCESSOR}_${CMAKE_BUILD_TYPE}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CJ_RUNTIME_STAGING_ROOT}/ar/${CMAKE_SYSTEM_PROCESSOR}_${CMAKE_BUILD_TYPE}")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CJ_RUNTIME_STAGING_ROOT}/bin/${CMAKE_SYSTEM_PROCESSOR}_${CMAKE_BUILD_TYPE}")
    file(MAKE_DIRECTORY "${CJ_RUNTIME_STAGING_ROOT}")
endmacro()
