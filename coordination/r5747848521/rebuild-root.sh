#!/bin/bash
set -euo pipefail
ulimit -c 0
R=/root/sym_cangjie_runtime_627_implement_r5747848521
S=$R/testable/runtime
LIB=$R/testable/build/runtime-staging/lib/x86_64_Release
TAG=${1:?tag}
OUT=$R/$TAG
mkdir -p "$OUT"
flags=(-std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti -fvisibility-inlines-hidden -DMRT_TESTABLE_INTERNALS=1 -DMRT_PRODUCT_TESTABLE_INTERNALS=1)
for inc in tests/gc_unit src src/Loader/BinaryFile src/Heap src/Heap/z/os/linux src/CJThread/src/runtime/schedule/include include third_party/third_party_bounds_checking_function/include; do flags+=(-I"$S/$inc"); done
flags+=(-I"$LIB/../../include")
start=$SECONDS
clang++ "${flags[@]}" -c "$S/tests/gc_unit/test_native_root_current.cpp" -o "$OUT/root.o" > "$OUT/build.log" 2>&1
objects=()
for o in "$R"/unit-initial/objects/main/*.o; do
 case "$o" in *-test_native_root_current.cpp.o) objects+=("$OUT/root.o");; *) objects+=("$o");; esac
done
clang++ "${flags[@]}" "${objects[@]}" -L"$LIB" -Wl,-rpath,"$LIB" -Wl,--exclude-libs,ALL -lcangjie-runtime -lboundscheck -o "$OUT/cj_gc_unit" >> "$OUT/build.log" 2>&1
sha256sum "$OUT/cj_gc_unit" "$LIB"/*.so > "$OUT/identity.sha256"
printf 'build_rc=0 wall=%s\n' "$((SECONDS-start))"
