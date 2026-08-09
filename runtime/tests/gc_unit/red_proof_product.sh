#!/usr/bin/env bash
# True red-proof: temporarily break product code, show tests go red, restore.
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
REGION_H="$SRC_ROOT/Heap/Allocator/RegionInfo.h"

backup_and_break() {
  local file="$1"
  local pattern="$2"
  local replacement="$3"
  local tag="$4"
  cp -a "$file" "$file.redproof.bak"
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
  grep -E 'FAIL|PASS|========|SEGV|段错误|GC_UNIT' "$OUT/red_${label}.log" || true
  # Accept either explicit FAIL lines or non-zero (crash on broken product path).
  if grep -q 'FAIL' "$OUT/red_${label}.log"; then
    echo "RED_PROOF_OK_$label (FAIL lines)"
    return 0
  fi
  if [[ $rc -ne 0 ]]; then
    echo "RED_PROOF_OK_$label (non-zero rc=$rc on broken product)"
    return 0
  fi
  echo "RED_PROOF_FAIL: suite stayed green for $label" >&2
  return 1
}

mkdir -p "$OUT"
trap 'restore "$REGION_H"' EXIT

# --- Red 1: BindLiveInfo0FromLiveIfNull no-op (installdomain product path) ---
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

# --- Red 2: GetRoute domain gate returns forged to-addr instead of nullptr ---
# Pre-fix behaviour: out-of-domain invents a route (ior root cause).
backup_and_break "$REGION_H" \
  '            return nullptr;
        }
        uint64_t preLiveBytes = GetPreLiveBytesInGhostRegion(fromAddress);
        MAddress toAddr = metadata.routeInfo.GetRoute(preLiveBytes);
        return from_region_addr(toAddr);
    }' \
  '            // red_proof: domain miss forges to-addr (pre GetRoute domain gate)
            return from_region_addr(0x20000000u);
        }
        uint64_t preLiveBytes = GetPreLiveBytesInGhostRegion(fromAddress);
        MAddress toAddr = metadata.routeInfo.GetRoute(preLiveBytes);
        return from_region_addr(toAddr);
    }' \
  "U3_domain"

run_expect_fail "U3_domain"
restore "$REGION_H"

echo "RED_PROOF_PRODUCT_OK: observed failures on product reverts U3+U4"
