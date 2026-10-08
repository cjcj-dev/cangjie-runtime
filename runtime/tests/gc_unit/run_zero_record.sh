#!/usr/bin/env bash
# Bounded product test build; use one ELF for green/cut/restored library pairs.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SRC=$ROOT/runtime/tests/gc_unit
: "${GCV2_RUNTIME_LIB_DIR:?}" "${GC_UNIT_OUT:?}"
mkdir -p "$GC_UNIT_OUT"
OUTPUT=$(python3 "$SRC/product_test_configuration.py" "$ROOT/runtime" "$GCV2_RUNTIME_LIB_DIR" "$GCV2_RUNTIME_LIB_DIR/../.." --resolve-root)
read -r testable gc ohos ndebug debug < <(python3 "$SRC/product_test_configuration.py" "$ROOT/runtime" "$GCV2_RUNTIME_LIB_DIR" "$OUTPUT" --with-debug)
flags=()
[[ $testable == 0 ]] || flags+=(-DMRT_TESTABLE_INTERNALS=1)
[[ $ndebug == 0 ]] || flags+=(-DNDEBUG)
[[ $debug == 0 ]] || flags+=(-DMRT_DEBUG=1)
clang++ -std=c++17 -O0 -g -fno-rtti -fvisibility-inlines-hidden -pthread "${flags[@]}" \
  -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" -I"$ROOT/runtime/src/Heap/z/os/linux" \
  -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" -I"$ROOT/runtime/include" \
  -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" -I"$OUTPUT/include" \
  "$SRC/zero_record_product.cpp" -L"$GCV2_RUNTIME_LIB_DIR" -Wl,-rpath,"$GCV2_RUNTIME_LIB_DIR" \
  -lcangjie-runtime -lboundscheck -ldl -o "$GC_UNIT_OUT/zero_record_product"
nm --defined-only "$GC_UNIT_OUT/zero_record_product" | c++filt > "$GC_UNIT_OUT/test-defined.txt"
nm --defined-only "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" | c++filt > "$GC_UNIT_OUT/product-defined.txt"
for symbol in 'StackInfo::CaptureRawFrame' 'StackFrameCursor::CollectReturnRegisterRoots' 'StackFrameCursor::ProcessManagedFrame'; do
  if grep -F "$symbol" "$GC_UNIT_OUT/test-defined.txt"; then exit 3; fi
  grep -F "$symbol" "$GC_UNIT_OUT/product-defined.txt"
done
if grep -E 'StackMapTable::|TableAPI::ResolveHeader|CompressedStackMapHead::GetStackMapEntry|BitsManager::GetBits' "$GC_UNIT_OUT/test-defined.txt"; then exit 3; fi
sha256sum "$GC_UNIT_OUT/zero_record_product" "$GCV2_RUNTIME_LIB_DIR/"*.so > "$GC_UNIT_OUT/build-artifacts.sha256"
