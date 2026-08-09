#!/usr/bin/env bash
# True red-proof: temporarily break product code, show tests go red, restore.
# Requires a built cj_gc_unit that already links product symbols.
# Usage (on kkk2, after sources synced):
#   bash runtime/tests/gc_unit/red_proof_product.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC_ROOT="$ROOT/runtime/src"
TEST="$ROOT/runtime/tests/gc_unit"
OUT="${GC_UNIT_OUT:-$TEST/build_standalone}"
RUNTIME_LIB_DIR="${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR}"
export GCV2_RUNTIME_LIB_DIR
export GC_UNIT_OUT="$OUT"

backup_and_break() {
  local file="$1"
  local pattern="$2"
  local replacement="$3"
  local tag="$4"
  cp -a "$file" "$file.redproof.bak"
  # shellcheck disable=SC2001
  if ! grep -q "$pattern" "$file"; then
    echo "RED_PROOF_SETUP_FAIL: pattern not found for $tag in $file" >&2
    mv "$file.redproof.bak" "$file"
    return 1
  fi
  # Use python for reliable multi-line-safe single substitution once.
  python3 - "$file" "$pattern" "$replacement" <<'PY'
import sys
path, pat, rep = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(path).read()
if pat not in text:
    sys.exit(2)
open(path, "w").write(text.replace(pat, rep, 1))
PY
  echo "BROKE $tag in $file"
}

restore() {
  local file="$1"
  if [[ -f "$file.redproof.bak" ]]; then
    mv "$file.redproof.bak" "$file"
    echo "RESTORED $file"
  fi
}

run_expect_fail() {
  local label="$1"
  set +e
  bash "$TEST/run_standalone.sh" >"$OUT/red_${label}.log" 2>&1
  local rc=$?
  set -e
  echo "--- red run $label rc=$rc ---"
  tail -40 "$OUT/red_${label}.log" || true
  if [[ $rc -eq 0 ]]; then
    echo "RED_PROOF_FAIL: expected failures for $label but suite was green" >&2
    return 1
  fi
  if ! grep -q 'FAIL' "$OUT/red_${label}.log"; then
    echo "RED_PROOF_FAIL: no FAIL lines for $label" >&2
    return 1
  fi
  echo "RED_PROOF_OK_$label"
}

mkdir -p "$OUT"

# --- Red 1: break RouteInfo::GetRoute domain-independent region1 (LiveInfo.cpp) ---
# Make GetRoute always return toRegion1StartAddress ignoring preLiveBytes bounds
# by forcing usedBytes check to always succeed with a huge value — better:
# break PlausibleManagedObjectGate tip-small-int reject.
COLLECTOR_CPP="$SRC_ROOT/Heap/Collector/Collector.cpp"
LIVEINFO_CPP="$SRC_ROOT/Heap/Collector/LiveInfo.cpp"

trap 'restore "$COLLECTOR_CPP"; restore "$LIVEINFO_CPP"' EXIT

# Red A: remove tip-small-int reject in product gate → U6 TipSmallIntRejected must fail.
backup_and_break "$COLLECTOR_CPP" \
  '} else if (tipAddr < kMinPlausibleTypeInfoAddr) {
        reason = "tip-small-int";
    } else if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {' \
  '} else if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {' \
  "U6_tip_small_int"

# Rebuild only object_gate + link against... wait: we need product .so rebuilt.
# For red proof of product, rebuild a tiny shared object from broken Collector.cpp is hard.
# Instead: patch LiveInfo.cpp GetRoute to ignore domain (always return to+pre) —
# but domain is in RegionInfo.h header. Patch BindLiveInfo0 to no-op.

# Actually for red A we need rebuilt runtime SO. Document that.
# Fall back red that works without full SO rebuild:
# 1) Patch header-inline RegionInfo::GetRoute domain gate to always call routeInfo.GetRoute
# 2) Patch header-inline BindLiveInfo0FromLiveIfNull to no-op

restore "$COLLECTOR_CPP"
REGION_H="$SRC_ROOT/Heap/Allocator/RegionInfo.h"

# Red 1: BindLiveInfo0FromLiveIfNull no-op → BindLiveInfo0 tests fail
backup_and_break "$REGION_H" \
  'void BindLiveInfo0FromLiveIfNull()
    {
        if (metadata.liveInfo0 != nullptr) {
            return;
        }
        LiveInfo* live = GetLiveInfo();
        if (live == nullptr) {
            return;
        }
        metadata.liveInfo0 = live;
        if (metadata.regionEnd0 == 0 || metadata.regionEnd0 < metadata.regionEnd) {
            metadata.regionEnd0 = metadata.regionEnd;
        }
    }' \
  'void BindLiveInfo0FromLiveIfNull()
    {
        // red_proof: installdomain broken — never bind ghost
        return;
    }' \
  "U4_bind"

run_expect_fail "U4_bind"
restore "$REGION_H"

# Red 2: GetRoute domain gate always allows (skip survived check)
backup_and_break "$REGION_H" \
  'LiveInfo* ghostLiveInfo = metadata.liveInfo0;
        if (ghostLiveInfo == nullptr || !ghostLiveInfo->IsSurvivedObject(offset)) {' \
  'LiveInfo* ghostLiveInfo = metadata.liveInfo0;
        if (false && (ghostLiveInfo == nullptr || !ghostLiveInfo->IsSurvivedObject(offset))) {' \
  "U3_domain"

run_expect_fail "U3_domain"
restore "$REGION_H"

# Red 3 (optional product .so path): tip-small-int — only if RED_PROOF_REBUILD_SO=1
if [[ "${RED_PROOF_REBUILD_SO:-0}" == "1" ]]; then
  backup_and_break "$COLLECTOR_CPP" \
    '} else if (tipAddr < kMinPlausibleTypeInfoAddr) {
        reason = "tip-small-int";
    } else if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {' \
    '} else if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {' \
    "U6_tip"
  echo "RED_PROOF_SO rebuild required externally for U6; skipping auto rebuild"
  restore "$COLLECTOR_CPP"
fi

echo "RED_PROOF_PRODUCT_OK: observed failures on product reverts U3+U4"
