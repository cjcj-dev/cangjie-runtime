#!/bin/bash
set -euo pipefail
ulimit -c 0
arm="$1"
OUT="/root/sym_cangjie_runtime_700_implement_r5723534433-debug/identity-preflight-$arm"
RUNTIME_LIB_DIR="/root/sym_cangjie_runtime_700_implement_r5723534433-shared-string/$arm/build/runtime-staging/lib/x86_64_Release"
mkdir -p "$OUT"
ln -sfn "/root/diff_379aae5a86ca/unit-$arm/cj_gc_unit" "$OUT/cj_gc_unit"
sha256sum "$OUT/cj_gc_unit" "$RUNTIME_LIB_DIR/libcangjie-runtime.so" > "$OUT/preflight-input.sha256"
STANDALONE_SYMBOLS=(
  _ZN12MapleRuntime8ZLiveMap5resetENS_13ZGenerationIdE
  _ZN12MapleRuntime8ZLiveMap13reset_segmentEm
)
STANDALONE_FULL_SYMBOLS=(
  _ZNK12MapleRuntime5ZPage19clone_for_promotionEv
  _ZN12MapleRuntime5ZMark15MarkEntryObjectEPNS_10BaseObjectERKNS_14MarkStackEntryEPNS_13MarkLiveCacheE
  _ZN12MapleRuntime5ZMark21MarkOldObjectIfActiveEPNS_10BaseObjectEb
)
# ZPage::CloneForPromotion and ZMark entry/active marking are out-of-line
# product functions. Full symbols exclude local copies as well as exports;
# matching product definitions keep retired names from making the guard inert.
STANDALONE_SYMBOL_DYN="$OUT/cj_gc_unit.dynamic-defined.txt"
STANDALONE_SYMBOL_FULL="$OUT/cj_gc_unit.full-defined.txt"
STANDALONE_PRODUCT_FULL="$OUT/standalone-product.full-defined.txt"
nm -D --defined-only "$OUT/cj_gc_unit" >"$STANDALONE_SYMBOL_DYN"
nm --defined-only "$OUT/cj_gc_unit" >"$STANDALONE_SYMBOL_FULL"
nm --defined-only "$RUNTIME_LIB_DIR/libcangjie-runtime.so" >"$STANDALONE_PRODUCT_FULL"
if ! /usr/bin/grep -Eq '[[:space:]]main$' "$STANDALONE_SYMBOL_FULL"; then
  echo "GC_UNIT_STANDALONE_SYMBOL_GUARD_BROKEN positive_control=main" >&2
  exit 7
fi
for symbol in "${STANDALONE_SYMBOLS[@]}"; do
  if /usr/bin/grep -F -q "$symbol" "$STANDALONE_SYMBOL_DYN"; then
    echo "GC_UNIT_STANDALONE_SYMBOL_GUARD_FAIL symbol=$symbol" >&2
    exit 7
  fi
done
for symbol in "${STANDALONE_FULL_SYMBOLS[@]}"; do
  if ! /usr/bin/grep -F -q "$symbol" "$STANDALONE_PRODUCT_FULL"; then
    echo "GC_UNIT_STANDALONE_PRODUCT_SYMBOL_MISSING symbol=$symbol" >&2
    exit 7
  fi
  if /usr/bin/grep -F -q "$symbol" "$STANDALONE_SYMBOL_FULL"; then
    echo "GC_UNIT_STANDALONE_SYMBOL_GUARD_FAIL symbol=$symbol" >&2
    exit 7
  fi
done
echo "GATE_STANDALONE_SYMBOLS_OK elf=$OUT/cj_gc_unit"

