#!/usr/bin/env bash
# Product debugger matrix for ZGC zDirector.cpp:608/612/632/802/807/830.
# Run on kkk2. No product hooks, rebuilt test components, or copied algorithms.
# Required: GC_UNIT_TEST_ELF, GCV2_RUNTIME_LIB_DIR, DIRECTOR_SOURCE, BUSY_OUT,
# BUSY_CPUSET (leased with cjops windows). Optional BUSY_SITES narrows a cut arm.
set -uo pipefail
ulimit -c 0
: "${GC_UNIT_TEST_ELF:?}" "${GCV2_RUNTIME_LIB_DIR:?}" "${DIRECTOR_SOURCE:?}"
: "${BUSY_OUT:?}" "${BUSY_CPUSET:?}"
script_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$BUSY_OUT"
export LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DIRECTOR_SOURCE
uptime > "$BUSY_OUT/uptime-before.txt"
start=$SECONDS
sha256sum "$GC_UNIT_TEST_ELF" "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" \
    "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$BUSY_OUT/identity.sha256"
nm --defined-only "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" > "$BUSY_OUT/product.full-nm.txt"
printf 'cpuset=%s\nscript=%s\nsource=%s\n' "$BUSY_CPUSET" "$script_dir/test_director_busy_gdb.py" "$DIRECTOR_SOURCE" > "$BUSY_OUT/recipe.txt"
run_case() {
    local site=$1 initial=$2 current=$3 resize=$4 equal=$5
    local name="$site-$initial-$current-r$resize-e$equal"
    BUSY_SITE=$site BUSY_INITIAL=$initial BUSY_CURRENT=$current BUSY_RESIZE=$resize BUSY_EQUAL=$equal \
      timeout 40 taskset -c "$BUSY_CPUSET" gdb -nx -batch \
      -ex "source $script_dir/test_director_busy_gdb.py" "$GC_UNIT_TEST_ELF" > "$BUSY_OUT/$name.log" 2>&1
    local rc=$?
    echo "$rc" > "$BUSY_OUT/$name.rc"
}
pids=()
for site in ${BUSY_SITES:-major minor minor_major merge select resize entry}; do
    for initial in ${BUSY_INITIALS:-0 1}; do
        for current in ${BUSY_CURRENTS:-0 1}; do
            run_case "$site" "$initial" "$current" 0 0 & pids+=("$!")
            if [[ "$site" == minor_major ]]; then
                run_case "$site" "$initial" "$current" 1 0 & pids+=("$!")
            elif [[ "$site" == resize ]]; then
                run_case "$site" "$initial" "$current" 1 1 & pids+=("$!")
            fi
        done
    done
done
for pid in "${pids[@]}"; do wait "$pid"; done
rc=0
for result in "$BUSY_OUT"/*.rc; do
    value=$(cat "$result")
    printf '%s rc=%s\n' "$(basename "$result" .rc)" "$value"
    if [[ "$value" != 0 ]]; then rc=1; fi
done
printf 'parallel_cases=%s wall=%ss\n' "${#pids[@]}" "$((SECONDS-start))" > "$BUSY_OUT/wall.txt"
uptime > "$BUSY_OUT/uptime-after.txt"
echo "$rc" > "$BUSY_OUT/run.rc"
exit "$rc"
