#!/usr/bin/env bash
# Deterministic exit/inventory interleaving against the linked product.
set -uo pipefail
ulimit -c 0
: "${GC_UNIT_TEST_ELF:?}" "${GCV2_RUNTIME_LIB_DIR:?}" "${DETACH_OUT:?}" "${DETACH_CPUSET:?}"
script_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$DETACH_OUT"
export LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
start=$SECONDS
uptime > "$DETACH_OUT/uptime-before.txt"
sha256sum "$GC_UNIT_TEST_ELF" "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$DETACH_OUT/identity.sha256"
nm --defined-only "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" > "$DETACH_OUT/product.full-nm.txt"
printf 'cpuset=%s\n' "$DETACH_CPUSET" > "$DETACH_OUT/recipe.txt"
timeout 60 taskset -c "$DETACH_CPUSET" gdb -nx -batch \
    -ex "source $script_dir/test_thread_detach_gdb.py" "$GC_UNIT_TEST_ELF" > "$DETACH_OUT/interleaving.log" 2>&1
rc=$?
printf '%s\n' "$rc" > "$DETACH_OUT/run.rc"
printf 'wall=%ss\n' "$((SECONDS-start))" > "$DETACH_OUT/wall.txt"
sha256sum -c "$DETACH_OUT/identity.sha256" > "$DETACH_OUT/identity-check-after.txt"
identity_rc=$?
uptime > "$DETACH_OUT/uptime-after.txt"
if [[ "$identity_rc" != 0 ]]; then exit "$identity_rc"; fi
exit "$rc"
