# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

if (NOT DEFINED REPOSITORY_DIR OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "REPOSITORY_DIR and OUTPUT_FILE are required")
endif()

set(runtime_commit "")
if (DEFINED COMMIT_OVERRIDE AND NOT COMMIT_OVERRIDE STREQUAL "")
    set(runtime_commit "${COMMIT_OVERRIDE}")
elseif (DEFINED ENV{CJ_RUNTIME_COMMIT} AND NOT "$ENV{CJ_RUNTIME_COMMIT}" STREQUAL "")
    set(runtime_commit "$ENV{CJ_RUNTIME_COMMIT}")
else()
    execute_process(
        COMMAND git -C "${REPOSITORY_DIR}" rev-parse HEAD
        RESULT_VARIABLE head_rc
        OUTPUT_VARIABLE runtime_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if (NOT head_rc EQUAL 0)
        set(runtime_commit "")
    endif()
endif()

if (runtime_commit STREQUAL "")
    set(runtime_commit "unknown")
endif()

execute_process(
    COMMAND git -C "${REPOSITORY_DIR}" status --porcelain
    RESULT_VARIABLE status_rc
    OUTPUT_VARIABLE runtime_dirty
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
# No git means status fails and the already-unambiguous "unknown" stamp stays clean.
if (status_rc EQUAL 0 AND NOT runtime_dirty STREQUAL "")
    set(runtime_commit "${runtime_commit}-dirty")
endif()

# Keep the configured override safe as a C++ string literal as well as a single stamp line.
string(REPLACE "\\" "\\\\" runtime_commit_escaped "${runtime_commit}")
string(REPLACE "\"" "\\\"" runtime_commit_escaped "${runtime_commit_escaped}")
string(REPLACE "\n" "\\n" runtime_commit_escaped "${runtime_commit_escaped}")
string(REPLACE "\r" "\\r" runtime_commit_escaped "${runtime_commit_escaped}")

get_filename_component(output_dir "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
set(output_tmp "${OUTPUT_FILE}.tmp")
file(WRITE "${output_tmp}"
    "// Generated at build time; do not edit.\n"
    "#if defined(__has_attribute)\n"
    "#  if __has_attribute(retain)\n"
    "#    define CJRT_RETAIN __attribute__((retain))\n"
    "#  else\n"
    "#    define CJRT_RETAIN\n"
    "#  endif\n"
    "#else\n"
    "#  define CJRT_RETAIN\n"
    "#endif\n"
    "extern \"C\" __attribute__((used, visibility(\"default\"))) CJRT_RETAIN\n"
    "const char g_cjRuntimeProvenance[] = \"CJRT-COMMIT:${runtime_commit_escaped}\";\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${output_tmp}" "${OUTPUT_FILE}"
    RESULT_VARIABLE copy_rc)
file(REMOVE "${output_tmp}")
if (NOT copy_rc EQUAL 0)
    message(FATAL_ERROR "failed to update ${OUTPUT_FILE}")
endif()
