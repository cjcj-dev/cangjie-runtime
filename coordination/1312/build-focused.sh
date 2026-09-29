#!/bin/bash
set -euo pipefail
ulimit -c 0
r=/root/sym_cangjie_runtime_1312_implement_r5899117285
src="$r/testable/runtime"
lib="$r/testable/build/runtime-staging/lib/x86_64_Release"
out="$r/focused-${1:-current}"
mkdir -p "$out"
root=$(python3 "$src/tests/gc_unit/product_test_configuration.py" "$src" "$lib" "$lib/../.." --resolve-root)
python3 "$src/tests/gc_unit/product_test_configuration.py" "$src" "$lib" "$root" > "$out/product-config.txt"
read -r testable gcunit ohos < "$out/product-config.txt"
flags=(-std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti -fvisibility-inlines-hidden)
[ "$testable" = 1 ] && flags+=(-DMRT_TESTABLE_INTERNALS=1 -DMRT_PRODUCT_TESTABLE_INTERNALS=1)
[ "$gcunit" = 1 ] && flags+=(-DMRT_GC_UNIT_TESTS=1)
for inc in tests/gc_unit src src/Loader/BinaryFile src/Heap src/Heap/z/os/linux src/CJThread/src/runtime/schedule/include include third_party/third_party_bounds_checking_function/include; do flags+=(-I"$src/$inc"); done
flags+=(-I"$root/include")
printf '%s\n' "${flags[@]}" > "$out/compile-flags.txt"
objects=()
for file in gc_worker_fixture gc_unit_main gc_cycle_sequence_fixture test_string_dedup; do
 objects+=("$out/$file.o")
 clang++ "${flags[@]}" -c "$src/tests/gc_unit/$file.cpp" -o "$out/$file.o" > "$out/$file.build.log" 2>&1 &
done
fail=0
for job in $(jobs -p); do wait "$job" || fail=1; done
[ "$fail" = 0 ] || { /usr/bin/grep -H 'error:' "$out/"*.build.log; exit 1; }
clang++ "${flags[@]}" "${objects[@]}" -L"$lib" -Wl,-rpath,"$lib" -Wl,--exclude-libs,ALL -lcangjie-runtime -lboundscheck -o "$out/dedup-unit"
sha256sum "$out/dedup-unit" "$lib"/*.so > "$out/artifacts.sha256"
nm --defined-only "$out/dedup-unit" | c++filt > "$out/defined.txt"
nm -u "$out/dedup-unit" | c++filt > "$out/undefined.txt"
env LD_LIBRARY_PATH="$lib" "$out/dedup-unit" --gtest_list_tests > "$out/registered.txt"
echo "FOCUSED_BUILD=$out rc=0"
