#!/usr/bin/env bash
# End-to-end product test for String.deduplicated(). The caller must provide
# rebuilt std.core module and shared-library directories from this tree.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/runtime/tests/string_dedup_product.cj"
OUT="${STRING_DEDUP_OUT:-$ROOT/runtime/tests/string_dedup_product_build}"
SDK="${STRING_DEDUP_SDK:?set STRING_DEDUP_SDK to the compiler SDK}"
MODULE_DIR="${STRING_DEDUP_STD_MODULE_DIR:?set STRING_DEDUP_STD_MODULE_DIR to rebuilt std modules}"
LIB_DIR="${STRING_DEDUP_STD_LIB_DIR:?set STRING_DEDUP_STD_LIB_DIR to rebuilt std shared libraries}"
CJC_BIN="$SDK/bin/cjc"
SDK_RUNTIME="$SDK/runtime/lib/linux_x86_64_cjnative"
SDK_TOOLS="$SDK/tools/lib"
SDK_LLVM="$SDK/third_party/llvm/lib"

if [[ ! -x "$CJC_BIN" ]]; then
  echo "STRING_DEDUP_RUNNER_FAIL: missing compiler $CJC_BIN" >&2
  exit 2
fi
if [[ ! -f "$MODULE_DIR/std.core.cjo" ]]; then
  echo "STRING_DEDUP_RUNNER_FAIL: missing rebuilt module $MODULE_DIR/std.core.cjo" >&2
  exit 2
fi
if [[ ! -f "$LIB_DIR/libcangjie-std-core.so" ]]; then
  echo "STRING_DEDUP_RUNNER_FAIL: missing rebuilt product $LIB_DIR/libcangjie-std-core.so" >&2
  exit 2
fi

mkdir -p "$OUT"
BIN="$OUT/string_dedup_product"
BUILD_LOG="$OUT/string_dedup_product.build.log"
RUN_LOG="$OUT/string_dedup_product.run.log"
LDD_LOG="$OUT/string_dedup_product.ldd.log"

LD_LIBRARY_PATH="$LIB_DIR:$SDK_RUNTIME:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$CJC_BIN" "$SRC" -O0 --dy-std --import-path "$MODULE_DIR" -L "$LIB_DIR" \
  -o "$BIN" >"$BUILD_LOG" 2>&1

LD_LIBRARY_PATH="$LIB_DIR:$SDK_RUNTIME:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ldd "$BIN" >"$LDD_LOG" 2>&1
if ! grep -Fq "$LIB_DIR/libcangjie-std-core.so" "$LDD_LOG"; then
  echo "STRING_DEDUP_RUNNER_FAIL: binary did not resolve rebuilt std.core product" >&2
  cat "$LDD_LOG" >&2
  exit 2
fi

set +e
LD_LIBRARY_PATH="$LIB_DIR:$SDK_RUNTIME:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  timeout 60s "$BIN" >"$RUN_LOG" 2>&1
rc=$?
set -e

cat "$RUN_LOG"
if [[ $rc -ne 0 ]]; then
  echo "STRING_DEDUP_RUNNER_FAIL rc=$rc" >&2
  exit "$rc"
fi
if ! grep -q '^STRING_DEDUP_RESULT failures=0$' "$RUN_LOG"; then
  echo "STRING_DEDUP_RUNNER_FAIL: missing success result" >&2
  exit 1
fi
echo "STRING_DEDUP_RUNNER_OK rc=0"
