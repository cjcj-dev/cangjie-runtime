# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# Included by pcre2's own build right after its project() call (CMAKE_PROJECT_INCLUDE, see Pcre2.cmake).
# pcre2 is built and shipped with the standard library (libpcre2-8.a objects are bundled into the
# std regex archives, libpcre2-8 is installed beside them), so like the std's own native code it keeps
# DWARF in every build type: -g leaves the build type's optimization flags unchanged, and its shared
# library is not linked with the toolchain's strip flag.
add_compile_options("$<$<COMPILE_LANGUAGE:C>:-g>")
if(STRIP_FLAG)
    separate_arguments(pcre2_shared_linker_flags UNIX_COMMAND "${CMAKE_SHARED_LINKER_FLAGS}")
    list(REMOVE_ITEM pcre2_shared_linker_flags "${STRIP_FLAG}")
    list(JOIN pcre2_shared_linker_flags " " CMAKE_SHARED_LINKER_FLAGS)
endif()
