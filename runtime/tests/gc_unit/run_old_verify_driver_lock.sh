#!/usr/bin/env bash
# Product-entry regression with read-only debugger observation; no runtime hooks.
# ZGC zGeneration.cpp:1155-1168. Reuse OLD_VERIFY_ELF for all mutation arms.
set -euo pipefail
ulimit -c 0
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SRC=$ROOT/runtime/tests/gc_unit
LIB=${GCV2_RUNTIME_LIB_DIR:?set product runtime and boundscheck directory}
OUT=${OLD_VERIFY_OUT:?set a fresh evidence directory}
mkdir -p "$OUT"
uptime > "$OUT/uptime-before.txt"
ELF=${OLD_VERIFY_ELF:-$OUT/old_verify_driver_lock}
if [[ -z ${OLD_VERIFY_ELF:-} ]]; then
    "${CXX:-clang++}" -std=gnu++17 -O0 -g -fstandalone-debug -pthread -fno-rtti \
        -I"$ROOT/runtime/src" -I"$ROOT/runtime/include" -I"$ROOT/runtime/src/Heap" \
        -I"$ROOT/runtime/src/Heap/z/os/linux" \
        -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
        -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
        -I"$LIB/../../include" "$SRC/old_verify_driver_lock.cpp" \
        -L"$LIB" -Wl,-rpath,"$LIB" -lcangjie-runtime -lboundscheck -ldl \
        -o "$ELF" > "$OUT/build.log" 2>&1
fi
sha256sum "$ELF" "$LIB/"*.so > "$OUT/identity.sha256"
nm --defined-only -C "$LIB/libcangjie-runtime.so" > "$OUT/product-defined.txt"
nm --defined-only -C "$ELF" > "$OUT/test-defined.txt"
nm -u -C "$ELF" > "$OUT/test-imports.txt"
for mode in concurrent solo disabled; do
    for n in 1 2; do (
        roots=1; arg=$mode
        if [[ $mode == disabled ]]; then roots=0; arg=concurrent; fi
        set +e
        env LD_LIBRARY_PATH="$LIB" ZVerifyRoots=$roots ZVerifyObjects=0 \
            timeout 90 gdb -nx -batch -x "$SRC/old_verify_driver_lock.gdb" \
            --args "$ELF" "$arg" > "$OUT/$mode-$n.log" 2>&1
        echo "$?" > "$OUT/$mode-$n.rc"
    ) & done
done
wait
failed=0
for mode in concurrent solo disabled; do
    for n in 1 2; do
        rc=$(cat "$OUT/$mode-$n.rc")
        echo "OLD_VERIFY_ARM mode=$mode sample=$n rc=$rc"
        /usr/bin/grep '^OLD_VERIFY_RESULT' "$OUT/$mode-$n.log" || true
        if [[ $rc != 0 ]]; then failed=$((failed+1)); fi
    done
done
uptime > "$OUT/uptime-after.txt"
echo "OLD_VERIFY_FAILED=$failed"
[[ $failed == 0 ]]
