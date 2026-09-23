#!/usr/bin/env bash
# Observe the real product retire path and the existing publication fixture.
set -uo pipefail
ulimit -c 0
: "${GC_UNIT_TEST_ELF:?}" "${GCV2_RUNTIME_LIB_DIR:?}"
: "${TLAB_LOCK_OUT:?}" "${TLAB_LOCK_CPUSET:?}"
script_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$TLAB_LOCK_OUT"
export LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
uptime > "$TLAB_LOCK_OUT/uptime-before.txt"
start=$SECONDS
sha256sum "$GC_UNIT_TEST_ELF" "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" \
    "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$TLAB_LOCK_OUT/identity.sha256"
nm --defined-only "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" > "$TLAB_LOCK_OUT/product.full-nm.txt"
strings "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" | /usr/bin/grep 'CJRT-COMMIT:' > "$TLAB_LOCK_OUT/stamp.txt"
printf 'cpuset=%s\n' "$TLAB_LOCK_CPUSET" > "$TLAB_LOCK_OUT/recipe.txt"
pids=()
for i in $(seq 1 "${TLAB_LOCK_N:-3}"); do
    (
      timeout 60 taskset -c "$TLAB_LOCK_CPUSET" gdb -nx -batch \
        -ex "source $script_dir/test_tlab_locks_gdb.py" "$GC_UNIT_TEST_ELF" > "$TLAB_LOCK_OUT/lock-$i.log" 2>&1
      echo "$?" > "$TLAB_LOCK_OUT/lock-$i.rc"
    ) & pids+=("$!")
done
(
    env GC_UNIT_FILTER=TLABUsage.BoundsAndDemand \
      timeout 60 taskset -c "$TLAB_LOCK_CPUSET" "$GC_UNIT_TEST_ELF" > "$TLAB_LOCK_OUT/unrelated.log" 2>&1
    echo "$?" > "$TLAB_LOCK_OUT/unrelated.rc"
) & pids+=("$!")
for pid in "${pids[@]}"; do wait "$pid"; done
rc=0
for file in "$TLAB_LOCK_OUT"/*.rc; do
    [[ "$(basename "$file")" == run.rc ]] && continue
    code=$(cat "$file")
    printf '%s rc=%s\n' "$(basename "$file")" "$code"
    [[ "$code" == 0 ]] || rc=1
done
printf 'parallel_cases=%s wall=%ss\n' "${#pids[@]}" "$((SECONDS-start))" > "$TLAB_LOCK_OUT/wall.txt"
uptime > "$TLAB_LOCK_OUT/uptime-after.txt"
echo "$rc" > "$TLAB_LOCK_OUT/run.rc"
exit "$rc"
