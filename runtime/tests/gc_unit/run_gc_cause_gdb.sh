#!/usr/bin/env bash
# Product cause matrix. All cases execute the same real runtime ELF and SO.
set -uo pipefail
ulimit -c 0
: "${GC_UNIT_TEST_ELF:?}" "${GCV2_RUNTIME_LIB_DIR:?}" "${CAUSE_SOURCE_ROOT:?}" "${CAUSE_OUT:?}" "${CAUSE_CPUSET:?}"
script_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$CAUSE_OUT"
export LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CAUSE_SOURCE_ROOT
uptime > "$CAUSE_OUT/uptime-before.txt"
start=$SECONDS
sha256sum "$GC_UNIT_TEST_ELF" "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$CAUSE_OUT/identity.sha256"
nm --defined-only "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" > "$CAUSE_OUT/product.full-nm.txt"
printf 'cpuset=%s\nsource=%s\n' "$CAUSE_CPUSET" "$CAUSE_SOURCE_ROOT" > "$CAUSE_OUT/recipe.txt"
run_case() {
  CAUSE_CASE=$1 timeout 45 taskset -c "$CAUSE_CPUSET" gdb -nx -batch \
    -ex "source $script_dir/test_gc_cause_gdb.py" "$GC_UNIT_TEST_ELF" > "$CAUSE_OUT/$1.log" 2>&1
  echo "$?" > "$CAUSE_OUT/$1.rc"
}
pids=()
for scenario in ${CAUSE_CASES:-major_timer minor_timer high_usage allocation_rate allocation_rate_static major_allocation_rate proactive warmup}; do
  run_case "$scenario" & pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid"; done
rc=0
for result in "$CAUSE_OUT"/*.rc; do
  value=$(cat "$result")
  printf '%s rc=%s\n' "$(basename "$result" .rc)" "$value"
  if [[ "$value" != 0 ]]; then rc=1; fi
done
printf 'parallel_cases=%s wall=%ss\n' "${#pids[@]}" "$((SECONDS-start))" > "$CAUSE_OUT/wall.txt"
uptime > "$CAUSE_OUT/uptime-after.txt"
echo "$rc" > "$CAUSE_OUT/run.rc"
exit "$rc"
