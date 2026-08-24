#!/usr/bin/env bash
# End-to-end proof that a Cangjie allocation binds through MCC_NewObject in the product SO.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit/m0_correlation_fixture.cj"
OUT="${GC_UNIT_OUT:-$ROOT/runtime/tests/gc_unit/build_standalone}"
RUNTIME_LIB_DIR="${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR}"
CJC_BIN="${CJC:-${CANGJIE_HOME:-}/bin/cjc}"

if [[ ! -x "$CJC_BIN" ]]; then
  echo "M0_CORRELATION_FIXTURE_FAIL: no matching cjc" >&2
  exit 2
fi
if [[ ! -f "$RUNTIME_LIB_DIR/libcangjie-runtime.so" ]]; then
  echo "M0_CORRELATION_FIXTURE_FAIL: missing product runtime" >&2
  exit 2
fi

mkdir -p "$OUT"
BIN="$OUT/m0_correlation_fixture"
BUILD_LOG="$OUT/m0_correlation_fixture.build.log"
RUN_LOG="$OUT/m0_correlation_fixture.run.log"
NM_LOG="$OUT/m0_correlation_fixture.nm.log"
SDK_RUNTIME="${CANGJIE_HOME:-}/runtime/lib/linux_x86_64_cjnative"
SDK_TOOLS="${CANGJIE_HOME:-}/tools/lib"
SDK_LLVM="${CANGJIE_HOME:-}/third_party/llvm/lib"

nm -D --defined-only "$RUNTIME_LIB_DIR/libcangjie-runtime.so" >"$NM_LOG"
for symbol in MRT_M0CorrTagNextAllocation MRT_M0CorrObserve MRT_M0CorrRelease \
              MRT_M0CorrSelectKeepLive MRT_M0CorrIsSelected; do
  count=$(/usr/bin/grep -c " $symbol@@\| $symbol$" "$NM_LOG" || true)
  if [[ $count -ne 1 ]]; then
    echo "M0_CORRELATION_FIXTURE_FAIL: export $symbol count=$count" >&2
    exit 3
  fi
done

LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$CJC_BIN" "$SRC" -O0 --static-std -o "$BIN" >"$BUILD_LOG" 2>&1

set +e
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  MRT_GCV2_DIAG=m0corr MRT_LOG_LEVEL=e cjHeapSize=64MB timeout 60s "$BIN" >"$RUN_LOG" 2>&1
rc=$?
set -e

binds=$(/usr/bin/grep -c '^\[M0CORR\] schema=1 rec=bind ' "$RUN_LOG" || true)
observations=$(/usr/bin/grep -c '^\[M0CORR\] schema=1 rec=observation ' "$RUN_LOG" || true)
releases=$(/usr/bin/grep -c '^\[M0CORR\] schema=1 rec=release ' "$RUN_LOG" || true)
footers=$(/usr/bin/grep -c '^\[M0CORR\] schema=1 rec=footer .* valid=1 ' "$RUN_LOG" || true)
done_count=$(/usr/bin/grep -c '^M0_CORRELATION_FIXTURE_OK selected=0$' "$RUN_LOG" || true)

allocation_token=$(sed -nE 's/^\[M0CORR\].* rec=bind .* allocation_token=([0-9]+) .*/\1/p' "$RUN_LOG")
claimed_token=$(sed -nE 's/^\[M0CORR\].* rec=observation .* claimed_allocation_token=([0-9]+) .*/\1/p' "$RUN_LOG")
consumer_allocation_token=$(sed -nE \
  's/^\[M0CORR\].* rec=observation .* consumer_allocation_token=([0-9]+) .*/\1/p' "$RUN_LOG")

if [[ $rc -ne 0 || $binds -ne 1 || $observations -ne 1 || $releases -ne 1 || $footers -ne 1 ||
      $done_count -ne 1 || -z "$allocation_token" || "$allocation_token" != "$claimed_token" ||
      "$allocation_token" != "$consumer_allocation_token" ]]; then
  echo "M0_CORRELATION_FIXTURE_FAIL rc=$rc binds=$binds observations=$observations releases=$releases " \
       "footers=$footers done=$done_count allocation_token=${allocation_token:-missing} " \
       "claimed=${claimed_token:-missing} consumer=${consumer_allocation_token:-missing}" >&2
  tail -40 "$RUN_LOG" >&2
  exit 1
fi

echo "M0_CORRELATION_FIXTURE_OK rc=0 binds=1 observations=1 releases=1 token_match=1 footer_valid=1"
