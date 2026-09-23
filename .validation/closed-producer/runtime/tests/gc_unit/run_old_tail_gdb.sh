#!/usr/bin/env bash
# Product thread test; see test_old_tail_gdb.py. Run on kkk2.
set -uo pipefail
ulimit -c 0
: "${GC_UNIT_TEST_ELF:?}" "${GCV2_RUNTIME_LIB_DIR:?}" "${DIRECTOR_SOURCE:?}"
: "${TAIL_OUT:?}" "${TAIL_CPUSET:?}"
script_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$TAIL_OUT"
export LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
uptime > "$TAIL_OUT/uptime-before.txt"
start=$SECONDS
sha256sum "$GC_UNIT_TEST_ELF" "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" \
    "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$TAIL_OUT/identity.sha256"
nm --defined-only "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" > "$TAIL_OUT/product.full-nm.txt"
printf 'cpuset=%s\n' "$TAIL_CPUSET" > "$TAIL_OUT/recipe.txt"
pids=()
for i in $(seq 1 "${TAIL_N:-3}"); do
    (
      timeout 45 taskset -c "$TAIL_CPUSET" gdb -nx -batch \
        -ex "source $script_dir/test_old_tail_gdb.py" "$GC_UNIT_TEST_ELF" > "$TAIL_OUT/tail-$i.log" 2>&1
      echo "$?" > "$TAIL_OUT/tail-$i.rc"
    ) & pids+=("$!")
done
(
    env GC_UNIT_FILTER=GcDirector.CycleUsesWorkerAccountingAndControlledClock \
      timeout 45 taskset -c "$TAIL_CPUSET" "$GC_UNIT_TEST_ELF" > "$TAIL_OUT/unrelated.log" 2>&1
    echo "$?" > "$TAIL_OUT/unrelated.rc"
) & pids+=("$!")
for pid in "${pids[@]}"; do wait "$pid"; done
rc=0
for file in "$TAIL_OUT"/*.rc; do
    code=$(cat "$file")
    printf '%s rc=%s\n' "$(basename "$file")" "$code"
    [[ "$code" == 0 ]] || rc=1
done
printf 'parallel_cases=%s wall=%ss\n' "${#pids[@]}" "$((SECONDS-start))" > "$TAIL_OUT/wall.txt"
uptime > "$TAIL_OUT/uptime-after.txt"
echo "$rc" > "$TAIL_OUT/run.rc"
exit "$rc"
