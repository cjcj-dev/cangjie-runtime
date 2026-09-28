# Check the mandatory loader dependency of the OHOS-shaped host runtime before
# accepting the configuration. Supply the same shim LD_LIBRARY_PATH that will
# be used at execution time. Do not cache success: shims can disappear between
# two invocations of cmake in the same build directory.
function(cj_check_ohos_host_libraries)
    set(_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../src/Signal/SignalStack.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source}")
    file(READ "${_source}" _signal_stack)
    # Extract the product's platform selection, not a second library-name list.
    # The other runtime dlopen sites take application input or probe optional
    # services (ArkTS/ROM namespace); they are not host startup prerequisites.
    string(REGEX MATCH
        "static void FindSymbolInLibc[^\n]*\n\\{\n([^}]+)    static void\\* libc"
        _declaration "${_signal_stack}")
    if (NOT _declaration)
        message(FATAL_ERROR "OHOS-host: cannot extract loader names from ${_source}")
    endif()
    set(_selection "${CMAKE_MATCH_1}")
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ohos-host-libraries")
    file(MAKE_DIRECTORY "${_dir}")
    file(WRITE "${_dir}/probe.cpp"
        "#include <dlfcn.h>\n#include <cstdio>\n#define __OHOS__ 1\nint main() {\n${_selection}\n"
        "void* handle = dlopen(libName, RTLD_LOCAL | RTLD_LAZY);\n"
        "if (!handle) { std::fprintf(stderr, \"OHOS_HOST_LIBRARY_MISSING %s: %s\\n\", libName, dlerror()); return 1; }\n"
        "std::printf(\"OHOS_HOST_LIBRARY_OK %s\\n\", libName); dlclose(handle); return 0;\n}\n")
    unset(_compiled CACHE)
    try_compile(_compiled "${_dir}/build" "${_dir}/probe.cpp"
        LINK_LIBRARIES ${CMAKE_DL_LIBS}
        COPY_FILE "${_dir}/probe" OUTPUT_VARIABLE _compile_output)
    if (NOT _compiled)
        message(FATAL_ERROR "OHOS-host loader probe failed to compile:\n${_compile_output}")
    endif()
    execute_process(COMMAND "${_dir}/probe" RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if (NOT "${_rc}" STREQUAL "0")
        message(FATAL_ERROR "OHOS-host loader self-check failed (rc=${_rc}):\n${_out}${_err}\n"
            "Provide the host runtime shims in LD_LIBRARY_PATH before configuring.")
    endif()
    message(STATUS "${_out}")
endfunction()
