# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.
if (WINDOWS_FLAG MATCHES 1)
    set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
    set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
    set(WARNING_FLAGS "-w")
    set(SECURE_CFLAG_FOR_SHARED_LIBRARY "-static ${SECURE_CFLAG_FOR_SHARED_LIBRARY}")
    set(CMAKE_C_FLAGS "${SECURE_CFLAG_FOR_SHARED_LIBRARY} ${WARNING_FLAGS}")
endif()

string(REPLACE "-fvisibility=hidden" "" CMAKE_C_FLAGS_NO_VISIBILITY "${CMAKE_C_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_NO_VISIBILITY}")
 
if (WINDOWS_FLAG MATCHES 1)
    # MSVCRT already provides most of secure functions,
    # so only files listed below need to be compiled from boundscheck library,
    # to avoid "multiple definition" error.
    # Refer to offiical manual of boundscheck library for more information.
    list(APPEND SRC_FILE src/memset_s.c src/snprintf_s.c src/vsnprintf_s.c src/secureprintoutput_a.c)

    include(CheckCSourceCompiles)
    check_c_source_compiles([[
        #include <stdarg.h>
        #include <stdio.h>
        int vsnprintf_s(char *dest, size_t destMax, size_t count, const char *format, va_list argList)
        {
            return 0;
        }
        int main(void) { return 0; }
    ]] BOUNDSCHECK_CAN_DEFINE_VSNPRINTF_S)
else()
    aux_source_directory(./src SRC_FILE)
endif()

if (MACOS_FLAG MATCHES 1)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -shared")
    endif()
endif()

# Build shared library.
add_library(boundscheck SHARED ${SRC_FILE})
if (WINDOWS_FLAG MATCHES 1 AND NOT BOUNDSCHECK_CAN_DEFINE_VSNPRINTF_S)
    target_compile_definitions(boundscheck PRIVATE SECUREC_ENABLE_VSNPRINTF=0)
endif()
