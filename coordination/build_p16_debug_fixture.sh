#!/usr/bin/env bash
set -eu
ulimit -c 0
src=/root/sym_cangjie_runtime_627_implement_r5744767112-verify-green/default/runtime
out=/root/sym_cangjie_runtime_627_implement_r5744767112-debug/${P16_DEBUG_FIXTURE_SUFFIX:-access2}
lib=/root/sym_cangjie_runtime_627_implement_r5744767112-debug/build/runtime-staging/lib/x86_64_Debug
mkdir -p "$out"
clang++ -std=gnu++17 -O0 -g -pthread -fno-rtti -I"$src/src" -I"$src/include" -I"$src/src/Heap" -I"$src/src/Heap/z/os/linux" -I"$src/src/CJThread/src/runtime/schedule/include" -I"$src/third_party/third_party_bounds_checking_function/include" -I"$lib/../../include" /root/sym_cangjie_runtime_627_implement_r5744767112/verify_debug_access.cpp -L"$lib" -Wl,-rpath,"$lib" -lcangjie-runtime -lboundscheck -ldl -o "$out/verify_debug_access" > "$out/build.log" 2>&1
sha256sum "$out/verify_debug_access" "$lib/"*.so > "$out/identity.sha256"
