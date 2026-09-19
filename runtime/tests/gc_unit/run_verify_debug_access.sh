#!/usr/bin/env bash
# Debug-only compiler-entry checks. Reuse one ELF across product cut/restored arms.
set -eu
ulimit -c 0
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
LIB="${GCV2_RUNTIME_LIB_DIR:?set the Debug runtime/bounds pair}"
OUT="${VERIFY_DEBUG_OUT:?set a new evidence directory}"
mkdir "$OUT"
uptime > "$OUT/uptime-before.txt"
ELF="${VERIFY_DEBUG_ELF:-$OUT/verify_debug_access}"
if [[ -z "${VERIFY_DEBUG_ELF:-}" ]]; then
  "${CXX:-clang++}" -std=gnu++17 -O0 -g -pthread -fno-rtti \
    -I"$ROOT/runtime/src" -I"$ROOT/runtime/include" -I"$ROOT/runtime/src/Heap" \
    -I"$ROOT/runtime/src/Heap/z/os/linux" \
    -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
    -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
    -I"$LIB/../../include" "$SRC/verify_debug_access.cpp" -L"$LIB" \
    -Wl,-rpath,"$LIB" -lcangjie-runtime -lboundscheck -ldl -o "$ELF" > "$OUT/build.log" 2>&1
fi
sha256sum "$ELF" "$LIB/"*.so > "$OUT/identity.sha256"
nm --defined-only "$LIB/libcangjie-runtime.so" > "$OUT/product-defined.txt"
nm --defined-only "$ELF" > "$OUT/test-defined.txt"
nm -u "$ELF" > "$OUT/test-imports.txt"
/usr/bin/grep -Eq ' main$' "$OUT/test-defined.txt"
/usr/bin/grep -Eq ' CJ_MCC_ReadStaticRef(@[^[:space:]]+)?$' "$OUT/product-defined.txt"
/usr/bin/grep -Eq ' CJ_MCC_ReadStaticRef(@[^[:space:]]+)?$' "$OUT/test-imports.txt"
if /usr/bin/grep -Eq ' CJ_MCC_ReadStaticRef(@[^[:space:]]+)?$' "$OUT/test-defined.txt"; then exit 79; fi
for mode in null valid invalid saferegion; do
  for n in 1 2 3; do (
    set +e
    env ZVerifyOops=1 ZVerifyRoots=0 ZVerifyMarking=0 ZVerifyRemembered=0 \
      LD_LIBRARY_PATH="$LIB" timeout 20 "$ELF" "$mode" > "$OUT/$mode-$n.log" 2>&1
    rc=$?; echo "$rc" > "$OUT/$mode-$n.rc"
  ) & done
done
wait
failed=0
for mode in null valid invalid saferegion; do
  for n in 1 2 3; do
    rc=$(cat "$OUT/$mode-$n.rc")
    expected=0; pattern=DEBUG_PRODUCT_ACCESS_RETURNED
    if [[ "$mode" = invalid ]]; then expected=134; pattern='Bad object'; fi
    if [[ "$mode" = saferegion ]]; then expected=134; pattern='WorldStopped'; fi
    result=PASS
    if [[ "$rc" != "$expected" ]] || ! /usr/bin/grep -Fq "$pattern" "$OUT/$mode-$n.log"; then result=FAIL; failed=$((failed+1)); fi
    echo "DEBUG_ACCESS_TARGET mode=$mode sample=$n rc=$rc expected=$expected result=$result"
  done
done
uptime > "$OUT/uptime-after.txt"
echo "DEBUG_ACCESS_TARGET_FAILED=$failed"
[[ "$failed" = 0 ]]
