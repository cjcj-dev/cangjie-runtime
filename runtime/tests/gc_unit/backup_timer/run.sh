#!/usr/bin/env bash
# kkk2 only. Link runtime_timer.cpp once, then reuse that ELF across SO arms.
set -uo pipefail
ulimit -c 0
: "${TIMER_ELF:?}" "${TIMER_SOURCE:?}" "${TIMER_OUT:?}" "${TIMER_CPUSET:?}" "${GCV2_RUNTIME_LIB_DIR:?}"
script_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$TIMER_OUT"
export LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export TIMER_SOURCE
uptime > "$TIMER_OUT/uptime-before.txt"
sha256sum "$TIMER_ELF" "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" \
    "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$TIMER_OUT/identity.sha256"
nm --defined-only "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" > "$TIMER_OUT/product.full-nm.txt"
printf 'cpuset=%s\nwait=%s\nsource=%s\n' "$TIMER_CPUSET" "${TIMER_WAIT_SECONDS:-241}" "$TIMER_SOURCE" > "$TIMER_OUT/recipe.txt"
start=$SECONDS
run_case() {
    local input=$1 explicit=$2 generation=$3
    local name="$input-$explicit-$generation"
    (
        unset cjBackupGCInterval
        export cjHeapSize=64MB cjProcessorNum=1 cjConcGCThreads=2 cjYoungGCThreads=2 cjOldGCThreads=2
        if [[ "$input" == env && "$explicit" == 1 ]]; then export cjBackupGCInterval=1s; fi
        TIMER_GENERATION=$generation TIMER_EXPLICIT=$explicit \
            timeout 290 taskset -c "$TIMER_CPUSET" gdb -nx -batch \
            -ex "source $script_dir/check_timer_gdb.py" --args "$TIMER_ELF" "$input" "$explicit"
    ) > "$TIMER_OUT/$name.log" 2>&1
    echo "$?" > "$TIMER_OUT/$name.rc"
}
pids=()
for input in api env; do
    for explicit in 0 1; do
        for generation in major minor; do
            run_case "$input" "$explicit" "$generation" & pids+=("$!")
        done
    done
done
for pid in "${pids[@]}"; do wait "$pid"; done
rc=0
for input in api env; do
    for explicit in 0 1; do
        for generation in major minor; do
            name="$input-$explicit-$generation"
            result=$(cat "$TIMER_OUT/$name.rc")
            echo "$name rc=$result"
            if [[ "$result" != 0 ]]; then rc=1; fi
        done
    done
done
printf 'parallel_cases=%s wall=%ss\n' "${#pids[@]}" "$((SECONDS-start))" > "$TIMER_OUT/wall.txt"
uptime > "$TIMER_OUT/uptime-after.txt"
echo "$rc" > "$TIMER_OUT/run.rc"
exit "$rc"
