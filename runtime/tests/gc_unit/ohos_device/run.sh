#!/usr/bin/env bash
# Actual AArch64 OHOS runtime + rebuilt OHOS musl, under qemu-user.
# This does not certify device services or a system-image loader.
set -euo pipefail
ulimit -c 0
src=$(cd "$(dirname "$0")/../../.." && pwd)
: "${OHOS_PUBLIC_SDK:?native SDK directory}"
: "${OHOS_RUNTIME_LIB:?product aarch64_Release directory}"
: "${OHOS_LOADER:?rebuilt OHOS musl libc.so}"
: "${OHOS_DEVICE_OUT:?private output directory}"
qemu=${QEMU_AARCH64:-qemu-aarch64}
mkdir -p "$OHOS_DEVICE_OUT/root/lib"
out=$(cd "$OHOS_DEVICE_OUT" && pwd)
# cp -L materializes input aliases. Never share writable symlinks with an SDK.
cp -L "$OHOS_LOADER" "$out/root/lib/ld-musl-aarch64.so.1"
cp -L "$OHOS_LOADER" "$out/root/lib/libc.so"
cp -L "$OHOS_RUNTIME_LIB/libcangjie-runtime.so" "$OHOS_RUNTIME_LIB/libboundscheck.so" "$out/root/lib/"
cp -L "$OHOS_PUBLIC_SDK/llvm/lib/aarch64-linux-ohos/libc++_shared.so" "$out/root/lib/"
for lib in libhilog_ndk.z.so libhitrace_ndk.z.so; do
    cp -L "$OHOS_PUBLIC_SDK/sysroot/usr/lib/aarch64-linux-ohos/$lib" "$out/root/lib/"
done
elf=${OHOS_DEVICE_ELF:-$out/cycle_probe}
if [[ -z ${OHOS_DEVICE_ELF:-} ]]; then
    "$OHOS_PUBLIC_SDK/llvm/bin/clang++" --target=aarch64-linux-ohos \
        --sysroot="$OHOS_PUBLIC_SDK/sysroot" -std=c++17 -O0 -g -fno-rtti \
        -D__OHOS__=1 -include string -pthread \
        -I"$src/src" -I"$src/src/Heap" -I"$src/src/Heap/z/os/linux" \
        -I"$src/src/CJThread/src/runtime/schedule/include" \
        -I"$src/include" -I"$OHOS_RUNTIME_LIB/../../include" \
        -I"$src/third_party/third_party_bounds_checking_function/include" \
        "$src/tests/gc_unit/ohos_device/cycle_probe.cpp" \
        -L"$OHOS_RUNTIME_LIB" -lcangjie-runtime -lboundscheck \
        -Wl,--allow-shlib-undefined -o "$elf"
fi
sha256sum "$elf" "$out/root/lib/"* > "$out/artifacts.sha256"
"$OHOS_PUBLIC_SDK/llvm/bin/llvm-readelf" -h -l "$elf" > "$out/elf.txt"
"$OHOS_PUBLIC_SDK/llvm/bin/llvm-readelf" -d "$out/root/lib/libcangjie-runtime.so" > "$out/runtime-needed.txt"
uptime > "$out/uptime-before.txt"
taskset -pc $$ > "$out/cpus.txt"
start=$SECONDS
rc=0
for mode in empty cycle; do
    set +e
    timeout 180 "$qemu" -L "$out/root" -E LD_LIBRARY_PATH=/lib "$elf" "$mode" > "$out/$mode.log" 2>&1
    result=$?
    set -e
    echo "$result" > "$out/$mode.rc"
    cat "$out/$mode.log"
    echo "OHOS_DEVICE_RESULT mode=$mode rc=$result"
    if [[ $result != 0 ]]; then rc=1; fi
done
echo "$((SECONDS-start))" > "$out/wall.txt"
uptime > "$out/uptime-after.txt"
exit "$rc"
