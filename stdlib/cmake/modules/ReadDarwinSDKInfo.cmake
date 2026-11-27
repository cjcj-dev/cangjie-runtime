# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
#
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

function(query_macos_sdk option output)
    execute_process(
        COMMAND xcrun --sdk macosx "${option}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE value
        ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT rc STREQUAL "0" OR value STREQUAL "")
        message(FATAL_ERROR "xcrun --sdk macosx ${option} failed (rc=${rc}, output='${value}'): ${error}")
    endif()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

if(CANGJIE_TARGET_SYSROOT)
    set(CANGJIE_MACOSX_SDK_PATH "${CANGJIE_TARGET_SYSROOT}")
elseif(CMAKE_SYSROOT)
    set(CANGJIE_MACOSX_SDK_PATH "${CMAKE_SYSROOT}")
else()
    query_macos_sdk(--show-sdk-path CANGJIE_MACOSX_SDK_PATH)
endif()
if(NOT IS_DIRECTORY "${CANGJIE_MACOSX_SDK_PATH}")
    message(FATAL_ERROR "macOS SDK directory does not exist: ${CANGJIE_MACOSX_SDK_PATH}")
endif()

# A copied SDK can differ from the one selected by xcrun.
set(CANGJIE_MACOSX_SDK_VERSION "")
set(sdk_settings "${CANGJIE_MACOSX_SDK_PATH}/SDKSettings.json")
set(sdk_version_pattern "^[0-9]+([.][0-9]+)?([.][0-9]+)?$")
if((CANGJIE_TARGET_SYSROOT OR CMAKE_SYSROOT) AND EXISTS "${sdk_settings}")
    cmake_minimum_required(VERSION 3.19) # string(JSON)
    file(READ "${sdk_settings}" settings)
    string(JSON version ERROR_VARIABLE json_error GET "${settings}" Version)
    if(NOT json_error AND version MATCHES "${sdk_version_pattern}")
        set(CANGJIE_MACOSX_SDK_VERSION "${version}")
    endif()
endif()
if(CANGJIE_MACOSX_SDK_VERSION STREQUAL "")
    query_macos_sdk(--show-sdk-version CANGJIE_MACOSX_SDK_VERSION)
    if(NOT CANGJIE_MACOSX_SDK_VERSION MATCHES "${sdk_version_pattern}")
        message(FATAL_ERROR "Invalid macOS SDK version from xcrun: '${CANGJIE_MACOSX_SDK_VERSION}'")
    endif()
endif()
message(STATUS "CANGJIE_MACOSX_SDK_PATH: ${CANGJIE_MACOSX_SDK_PATH}")
message(STATUS "CANGJIE_MACOSX_SDK_VERSION: ${CANGJIE_MACOSX_SDK_VERSION}")
