# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# build.py's --target-sysroot takes precedence over CMake SDK settings.
# Resolve before project() enables languages, including nested try_compile.
if(CANGJIE_TARGET_SYSROOT)
    set(CMAKE_OSX_SYSROOT "${CANGJIE_TARGET_SYSROOT}")
elseif(NOT CMAKE_OSX_SYSROOT)
    if(CMAKE_IOS_SDK_ROOT)
        set(CMAKE_OSX_SYSROOT "${CMAKE_IOS_SDK_ROOT}")
    else()
        execute_process(
            COMMAND xcrun --sdk ${IOS_SDK_NAME} --show-sdk-path
            RESULT_VARIABLE IOS_SDK_RESULT
            OUTPUT_VARIABLE CMAKE_OSX_SYSROOT
            ERROR_VARIABLE IOS_SDK_ERROR
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT IOS_SDK_RESULT STREQUAL "0" OR NOT CMAKE_OSX_SYSROOT)
            message(FATAL_ERROR "Cannot resolve ${IOS_SDK_NAME} SDK: ${IOS_SDK_ERROR}")
        endif()
    endif()
endif()
set(CMAKE_IOS_SDK_ROOT "${CMAKE_OSX_SYSROOT}")
# Propagate caller settings to CMake's compiler checks.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CANGJIE_TARGET_SYSROOT CMAKE_IOS_SDK_ROOT)
message(STATUS "iOS SDK (${IOS_SDK_NAME}): ${CMAKE_OSX_SYSROOT}")
