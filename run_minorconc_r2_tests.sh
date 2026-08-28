#!/usr/bin/env bash
set -uo pipefail

TEST_ELF=${TEST_ELF:?need cj_gc_unit path}
PRODUCT_DIR=${PRODUCT_DIR:?need product lib directory}
EVIDENCE=${EVIDENCE:?need evidence directory}
CORES=${CORES:-0-15}
GC_INTERVAL=${GC_INTERVAL:-5ms}
GC_HEAP_SIZE=${GC_HEAP_SIZE:-64MB}
mkdir -p "$EVIDENCE"
uptime > "$EVIDENCE/uptime.before"
sha256sum "$TEST_ELF" "$PRODUCT_DIR/libcangjie-runtime.so" "$PRODUCT_DIR/libboundscheck.so" \
  > "$EVIDENCE/identity"

tests=(
  YoungConc.Y2yAfterReleaseBatchForcesContinueAndReachesClosure
  YoungConc.SatbAfterWorkerTerminationUsesBoundedMarkEndContinue
  YoungConc.RuntimeMutatorCreateDuringActiveEpochIsBornClean
  YoungConc.RuntimeMutatorDestroyDuringActiveEpochDefersStorage
  YoungConc.RuntimeMutatorActiveEpochCreateDestroyInterleavingIsSerialized
  YoungConc.RuntimeMutatorDestroyInactiveCheckCannotCrossEpochPublish
  YoungConc.ForcedEpochHandshakeReportsPositiveRequested
  YoungConc.ForcedEpochHandshakeCoversAllParticipantSources
  YoungConc.FinalizerCreateDuringActiveEpochIsBornClean
  YoungConc.FinalizerEndDuringActiveEpochDefersStorage
)

printf 'iteration\ttest\trc\n' > "$EVIDENCE/results.tsv"
overall=0
for iteration in 1 2 3 4 5; do
  for test_name in "${tests[@]}"; do
    label="n${iteration}_${test_name#YoungConc.}"
    env MRT_GC_LOG=1 MRT_LOG_LEVEL=e cjGCInterval="$GC_INTERVAL" cjHeapSize="$GC_HEAP_SIZE" \
      LD_LIBRARY_PATH="$PRODUCT_DIR" taskset -c "$CORES" timeout 120 \
      "$TEST_ELF" "--gtest_filter=$test_name" \
      > "$EVIDENCE/$label.out" 2> "$EVIDENCE/$label.err"
    rc=$?
    printf '%s\t%s\t%s\n' "$iteration" "$test_name" "$rc" >> "$EVIDENCE/results.tsv"
    if [[ $rc -ne 0 ]]; then
      overall=1
    fi
  done
done
uptime > "$EVIDENCE/uptime.after"
sha256sum "$EVIDENCE"/*.out "$EVIDENCE"/*.err > "$EVIDENCE/SHA256SUMS"
exit "$overall"
