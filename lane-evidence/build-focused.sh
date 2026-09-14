#!/bin/bash
ulimit -c 0
set -u
root=/root/sym_cangjie_runtime_571_implement_r5668109509-baseline/testable
cd "$root"
lib=$root/build/runtime-staging/lib/x86_64_Release
out=$root/native-focused
mkdir -p "$out"
start=$SECONDS
flags=(-std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti -fvisibility-inlines-hidden -DMRT_TESTABLE_INTERNALS=1 -DMRT_PRODUCT_TESTABLE_INTERNALS=1 -DMRT_GC_UNIT_TESTS=1 -Iruntime/tests/gc_unit -Iruntime/src -Iruntime/src/Heap -Iruntime/src/CJThread/src/runtime/schedule/include -Iruntime/include -Iruntime/third_party/third_party_bounds_checking_function/include -Ibuild/runtime-staging/include)
object=$(ls "$root/native-tests/objects/main/"*-test_native_root_current.cpp.o)
clang++ "${flags[@]}" -c runtime/tests/gc_unit/test_native_root_current.cpp -o "$object" > "$out/test_native_root_current.cpp.log" 2>&1 || { cat "$out/test_native_root_current.cpp.log"; exit 1; }
clang++ "${flags[@]}" "$root/native-tests/objects/main/"*.o -L"$lib" -Wl,-rpath,"$lib" -Wl,--exclude-libs,ALL -lcangjie-runtime -lboundscheck -o "$out/native-root" > "$out/link.log" 2>&1
rc=$?; echo BUILD_RC=$rc wall=$((SECONDS-start)); [ "$rc" = 0 ] || exit "$rc"
sha256sum "$out/native-root" "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" > "$out/identity.sha256"
for name in MinorPublication MajorSeed ReadOnlyNonHeapBoundary; do
 (LD_LIBRARY_PATH="$lib" "$out/native-root" --gtest_filter=NativeRootCurrent.$name > "$out/$name.log" 2>&1; echo $? > "$out/$name.rc") &
done
wait
for name in MinorPublication MajorSeed ReadOnlyNonHeapBoundary; do echo "$name rc=$(cat "$out/$name.rc")"; tail -10 "$out/$name.log"; done
