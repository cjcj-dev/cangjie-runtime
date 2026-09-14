#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_581_implement_r5668277053-final3
cd "$root" || exit 2
export GC_UNIT_JOBS=192 GC_UNIT_BUILD_JOBS=192
uptime > tests-uptime-before.txt
sha256sum unit-default/cj_gc_unit unit-default/cj_gc_forwarding_publication_unit > elf.sha256
nm --defined-only unit-default/cj_gc_forwarding_publication_unit | c++filt > test.full-defined.txt
nm -u unit-default/cj_gc_forwarding_publication_unit | c++filt > test.undefined.txt
for arm in producer promotion phase consumer entry restored; do
 (
    lib="$root-$arm/default/build/runtime-staging/lib/x86_64_Release"
    sha256sum "$lib"/*.so > "$arm-run-so.sha256"
    strings "$lib/libcangjie-runtime.so" | /usr/bin/grep 'CJRT-COMMIT:' > "$arm-stamp.txt"
    nm --defined-only "$lib/libcangjie-runtime.so" | c++filt > "$arm-product.full-defined.txt"
    start=$SECONDS
    taskset -c 32-63 bash default/runtime/tests/gc_unit/run_parallel_tests.sh "$root/unit-default/cj_gc_unit" "$root/unit-default/cj_gc_forwarding_publication_unit" "$root/run-$arm" "$lib" > "run-$arm.log" 2>&1
    echo $? > "run-$arm.rc"
    echo $((SECONDS-start)) > "run-$arm.wall"
 ) &
done
(
 ohos="$root-ohos/default"
 cd "$ohos" || exit 2
 git init > "$root/ohos-git.log" 2>&1 &&
 git fetch --depth=1 https://github.com/cjcj-dev/cangjie-runtime.git 4ae08d2033af7d35d0466264b5b601313527dc48 >> "$root/ohos-git.log" 2>&1 &&
 git reset --soft FETCH_HEAD >> "$root/ohos-git.log" 2>&1
 echo $? > "$root/ohos-git.rc"
 export GCV2_RUNTIME_LIB_DIR="$ohos/build/runtime-staging/lib/x86_64_Release" MRT_GC_UNIT_OHOS_HOST=1 GC_UNIT_OUT="$root/unit-ohos"
 start=$SECONDS
 taskset -c 32-63 bash "$ohos/runtime/tests/gc_unit/run_standalone.sh" > "$root/unit-ohos.log" 2>&1
 echo $? > "$root/unit-ohos.rc"
 echo $((SECONDS-start)) > "$root/unit-ohos.wall"
) &
wait
uptime > tests-uptime-after.txt
