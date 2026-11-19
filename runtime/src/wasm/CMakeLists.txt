# Standalone build of the wasm32-wasi Cangjie runtime archive.
#   cmake -G Ninja -S runtime/src/wasm -B <build> \
#     -DCMAKE_C_COMPILER=<cjcj-llvm>/bin/clang -DCMAKE_AR=<cjcj-llvm>/bin/llvm-ar \
#     -DWASI_SYSROOT=<pinned wasi-sysroot> -DCMAKE_INSTALL_PREFIX=<sdk>
cmake_minimum_required(VERSION 3.16)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(cangjie_runtime_wasm C)
if(NOT WASI_SYSROOT)
  message(FATAL_ERROR "WASI_SYSROOT (the pinned wasi-sysroot) is required")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_library(cangjie-runtime STATIC CjWasmRuntime.c CjWasmHeap.c)
target_compile_options(cangjie-runtime PRIVATE --target=wasm32-wasi --sysroot=${WASI_SYSROOT}
  -O2 -std=c11 -Wall -Wextra -Werror -fno-exceptions)
install(TARGETS cangjie-runtime ARCHIVE DESTINATION runtime/lib/wasi_wasm32_cjnative)
