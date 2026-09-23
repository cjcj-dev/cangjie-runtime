#!/usr/bin/env bash
# kkk2 only. Preserve one ELF for all SO arms.
set -uo pipefail
ulimit -c 0
: "${SCOPE_ELF:?}" "${SCOPE_OUT:?}" "${SCOPE_CPUSET:?}" "${GCV2_RUNTIME_LIB_DIR:?}"
script_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$SCOPE_OUT"
export LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
uptime > "$SCOPE_OUT/uptime-before.txt"
sha256sum "$SCOPE_ELF" "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" \
    "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$SCOPE_OUT/identity.sha256"
nm --defined-only "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" > "$SCOPE_OUT/product.full-nm.txt"
nm --defined-only "$SCOPE_ELF" > "$SCOPE_OUT/fixture.full-nm.txt"
printf 'cpuset=%s\n' "$SCOPE_CPUSET" > "$SCOPE_OUT/recipe.txt"
start=$SECONDS
pids=()
for n in 1 2 3; do
    (
        timeout 120 taskset -c "$SCOPE_CPUSET" gdb -nx -batch \
            -ex "source $script_dir/check_scope_gdb.py" --args "$SCOPE_ELF" > "$SCOPE_OUT/run-$n.log" 2>&1
        echo "$?" > "$SCOPE_OUT/run-$n.rc"
    ) & pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid"; done
rc=0
for n in 1 2 3; do
    result=$(cat "$SCOPE_OUT/run-$n.rc")
    echo "run=$n rc=$result"
    if [[ "$result" != 0 ]]; then rc=1; fi
done
printf 'parallel_cases=3 wall=%ss\n' "$((SECONDS-start))" > "$SCOPE_OUT/wall.txt"
uptime > "$SCOPE_OUT/uptime-after.txt"
echo "$rc" > "$SCOPE_OUT/run.rc"
exit "$rc"
