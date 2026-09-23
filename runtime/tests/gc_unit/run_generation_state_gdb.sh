#!/usr/bin/env bash
# Real-entry generation/forwarding/signal observations; reuse one ELF for cuts.
set -uo pipefail
ulimit -c 0
: "${GC_UNIT_TEST_ELF:?}" "${GCV2_RUNTIME_LIB_DIR:?}" "${GENERATION_OUT:?}" "${GENERATION_CPUSET:?}"
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$GENERATION_OUT"
export LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
uptime > "$GENERATION_OUT/uptime-before.txt"
sha256sum "$GC_UNIT_TEST_ELF" "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$GENERATION_OUT/identity.sha256"
nm --defined-only -C "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" > "$GENERATION_OUT/product.full-nm.txt"
printf 'cpuset=%s\n' "$GENERATION_CPUSET" > "$GENERATION_OUT/recipe.txt"
start=$SECONDS
for mode in ${GENERATION_MODES:-forwarding signal phase seqnum activity}; do (
    GENERATION_OBSERVE=$mode timeout 90 taskset -c "$GENERATION_CPUSET" gdb -nx -batch \
        -ex "source $source_dir/test_generation_state_gdb.py" "$GC_UNIT_TEST_ELF" > "$GENERATION_OUT/$mode.log" 2>&1
    rc=$?
    if [[ $mode == signal ]]; then
        grep -q 'rec=crash .*gc_phase=Relocate gc_kind=fix in_par_fix=1' "$GENERATION_OUT/$mode.log" || rc=1
    fi
    echo "$rc" > "$GENERATION_OUT/$mode.rc"
) & done
wait
rc=0
for mode in ${GENERATION_MODES:-forwarding signal phase seqnum activity}; do
    value=$(cat "$GENERATION_OUT/$mode.rc")
    echo "GENERATION_ARM mode=$mode rc=$value"
    grep -E 'GENERATION_(OBSERVER_RESULT|SIGNAL_DELIVERY)|rec=crash' "$GENERATION_OUT/$mode.log" || true
    [[ $value == 0 ]] || rc=1
done
printf 'parallel_modes=forwarding,signal,phase,seqnum,activity wall=%ss\n' "$((SECONDS-start))" > "$GENERATION_OUT/wall.txt"
uptime > "$GENERATION_OUT/uptime-after.txt"
echo "$rc" > "$GENERATION_OUT/run.rc"
exit "$rc"
