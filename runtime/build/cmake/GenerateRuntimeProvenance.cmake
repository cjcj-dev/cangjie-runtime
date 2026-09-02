# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

if (NOT DEFINED REPOSITORY_DIR OR NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "REPOSITORY_DIR, SOURCE_DIR and OUTPUT_FILE are required")
endif()

# A declaration is build metadata, never the source identity. Keep it only for
# callers that still annotate tarball builds with CJ_RUNTIME_COMMIT.
set(runtime_declared "")
if (DEFINED COMMIT_OVERRIDE AND NOT COMMIT_OVERRIDE STREQUAL "")
    set(runtime_declared "${COMMIT_OVERRIDE}")
elseif (DEFINED ENV{CJ_RUNTIME_COMMIT} AND NOT "$ENV{CJ_RUNTIME_COMMIT}" STREQUAL "")
    set(runtime_declared "$ENV{CJ_RUNTIME_COMMIT}")
endif()
if (runtime_declared STREQUAL "")
    set(runtime_declared "none")
endif()

set(runtime_commit "")
if (EXISTS "${REPOSITORY_DIR}/.git")
    execute_process(
        COMMAND git -C "${REPOSITORY_DIR}" rev-parse HEAD
        RESULT_VARIABLE head_rc
        OUTPUT_VARIABLE runtime_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if (NOT head_rc EQUAL 0)
        message(FATAL_ERROR "${REPOSITORY_DIR}/.git exists but HEAD cannot be read")
    endif()

    execute_process(
        COMMAND git -C "${REPOSITORY_DIR}" status --porcelain
        RESULT_VARIABLE status_rc
        OUTPUT_VARIABLE runtime_dirty
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if (NOT status_rc EQUAL 0)
        message(FATAL_ERROR "${REPOSITORY_DIR}/.git exists but status cannot be read")
    endif()
    if (NOT runtime_dirty STREQUAL "")
        set(runtime_commit "${runtime_commit}-dirty")
    endif()
else()
    # Hash source-owned product inputs, not output from a previous build. Paths
    # are part of the digest so rename-only source changes also change identity.
    set(runtime_source_files
        "${SOURCE_DIR}/CMakeLists.txt"
        "${SOURCE_DIR}/SetupAr.cmake"
        "${SOURCE_DIR}/build.py"
        "${SOURCE_DIR}/config.cmake")
    foreach(source_root build include src tests)
        file(GLOB_RECURSE source_root_files
            LIST_DIRECTORIES false
            RELATIVE "${SOURCE_DIR}"
            "${SOURCE_DIR}/${source_root}/*")
        foreach(source_file IN LISTS source_root_files)
            if (source_file MATCHES "(^|/)(CMakeFiles|CMakebuild[^/]*|__pycache__|output)(/|$)" OR
                source_file MATCHES "^build/cjthread_build/" OR
                source_file MATCHES "\\.(pyc|pyo)$")
                continue()
            endif()
            list(APPEND runtime_source_files "${SOURCE_DIR}/${source_file}")
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES runtime_source_files)
    list(SORT runtime_source_files)

    set(runtime_source_digest_input "")
    set(runtime_source_count 0)
    foreach(source_file IN LISTS runtime_source_files)
        if (NOT EXISTS "${source_file}" OR IS_DIRECTORY "${source_file}")
            continue()
        endif()
        file(RELATIVE_PATH source_path "${SOURCE_DIR}" "${source_file}")
        file(SHA256 "${source_file}" source_sha256)
        string(LENGTH "${source_path}" source_path_length)
        string(APPEND runtime_source_digest_input
            "${source_path_length}:${source_path}:${source_sha256}\n")
        math(EXPR runtime_source_count "${runtime_source_count} + 1")
    endforeach()
    if (runtime_source_count EQUAL 0)
        message(FATAL_ERROR "no runtime source inputs found under ${SOURCE_DIR}")
    endif()
    string(SHA256 runtime_source_sha256 "${runtime_source_digest_input}")
    set(runtime_commit "src-${runtime_source_sha256}")
endif()

# Keep both lines safe as C++ string literal contents and as separate strings(1) records.
foreach(value_name runtime_commit runtime_declared)
    string(REPLACE "\\" "\\\\" ${value_name}_escaped "${${value_name}}")
    string(REPLACE "\"" "\\\"" ${value_name}_escaped "${${value_name}_escaped}")
    string(REPLACE "\n" "\\n" ${value_name}_escaped "${${value_name}_escaped}")
    string(REPLACE "\r" "\\r" ${value_name}_escaped "${${value_name}_escaped}")
endforeach()

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
    "const char g_cjRuntimeProvenance[] = \"CJRT-COMMIT:${runtime_commit_escaped}\\n"
    "CJRT-DECLARED:${runtime_declared_escaped}\";\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${output_tmp}" "${OUTPUT_FILE}"
    RESULT_VARIABLE copy_rc)
file(REMOVE "${output_tmp}")
if (NOT copy_rc EQUAL 0)
    message(FATAL_ERROR "failed to update ${OUTPUT_FILE}")
endif()
