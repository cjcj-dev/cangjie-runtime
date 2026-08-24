#!/usr/bin/env bash
# Product-SO acceptance for explicit ELF unload quiescence.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/runtime/tests"
OUT="${ELF_UNLOAD_OUT:-$ROOT/runtime/output/elf_unload_quiescence}"
RUNTIME_LIB_DIR="${ELF_UNLOAD_RUNTIME_LIB_DIR:-}"
CANGJIE_SDK="${ELF_UNLOAD_CANGJIE_SDK:-}"
CXX="${CXX:-clang++}"

if [[ -z "$RUNTIME_LIB_DIR" || ! -f "$RUNTIME_LIB_DIR/libcangjie-runtime.so" ]]; then
    echo "error: ELF_UNLOAD_RUNTIME_LIB_DIR must contain libcangjie-runtime.so" >&2
    exit 2
fi
if [[ -z "$CANGJIE_SDK" || ! -x "$CANGJIE_SDK/bin/cjc" ]]; then
    echo "error: ELF_UNLOAD_CANGJIE_SDK must contain bin/cjc" >&2
    exit 2
fi
mkdir -p "$OUT"

cjEnableGC=1 CANGJIE_HOME="$CANGJIE_SDK" "$CANGJIE_SDK/bin/cjc" \
    "$SRC/elf_unload_quiescence_plugin.cj" --output-type=dylib \
    -o "$OUT/libelfunloadprobe.so"
cjEnableGC=1 CANGJIE_HOME="$CANGJIE_SDK" "$CANGJIE_SDK/bin/cjc" \
    "$SRC/elf_unload_queue_blocker.cj" --output-type=dylib \
    -o "$OUT/libelfunloadqueueblocker.so"
build_snapshot_dylib() {
    local source="$1"
    local output="$2"
    cjEnableGC=1 CANGJIE_HOME="$CANGJIE_SDK" "$CANGJIE_SDK/bin/cjc" \
        "$SRC/$source" --output-type=dylib -o "$OUT/$output"
}
build_snapshot_dylib package_snapshot_leaf_r.cj libsnapshot_leaf_r.so
build_snapshot_dylib package_snapshot_leaf_t.cj libsnapshot_leaf_t.so
build_snapshot_dylib package_snapshot_branch.cj libsnapshot_branch.so
build_snapshot_dylib package_snapshot_sibling.cj libsnapshot_sibling.so
build_snapshot_dylib package_snapshot_root.cj libsnapshot_root.so
cjEnableGC=1 CANGJIE_HOME="$CANGJIE_SDK" "$CANGJIE_SDK/bin/cjc" \
    "$SRC/package_snapshot_public.cj" -o "$OUT/package_snapshot_public"

MARKER_SYMBOL="$(nm -D --defined-only "$OUT/libelfunloadprobe.so" | \
    awk '$3 ~ /unloadMarker/ { print $3; exit }')"
NATIVE_BLOCK_SYMBOL="$(nm -D --defined-only "$OUT/libelfunloadprobe.so" | \
    awk '$3 ~ /unloadNativeBlock/ { print $3; exit }')"
QUEUED_TASK_SYMBOL="$(nm -D --defined-only "$OUT/libelfunloadprobe.so" | \
    awk '$3 ~ /unloadQueuedTask/ { print $3; exit }')"
SPAWN_ACTIVE_SYMBOL="$(nm -D --defined-only "$OUT/libelfunloadprobe.so" | \
    awk '$3 ~ /unloadSpawnActive/ { print $3; exit }')"
BLOCKER_SYMBOL="$(nm -D --defined-only "$OUT/libelfunloadqueueblocker.so" | \
    awk '$3 ~ /queueBlocker/ { print $3; exit }')"
if [[ -z "$MARKER_SYMBOL" || -z "$NATIVE_BLOCK_SYMBOL" || -z "$QUEUED_TASK_SYMBOL" ||
      -z "$SPAWN_ACTIVE_SYMBOL" || -z "$BLOCKER_SYMBOL" ]]; then
    echo "error: required unload probe dynamic symbol not found" >&2
    exit 2
fi

"$CXX" -std=gnu++17 -O2 -g -Wall -Wextra -pthread \
    -I"$ROOT/runtime/src" -I"$ROOT/runtime/include" \
    "$SRC/elf_unload_quiescence_harness.cpp" \
    -L"$RUNTIME_LIB_DIR" -Wl,-rpath,"$RUNTIME_LIB_DIR" -Wl,--export-dynamic \
    -lcangjie-runtime -lboundscheck -ldl -o "$OUT/elf_unload_quiescence"

echo "PRODUCT_RUNTIME=$RUNTIME_LIB_DIR/libcangjie-runtime.so"
echo "PLUGIN=$OUT/libelfunloadprobe.so"
echo "MARKER_SYMBOL=$MARKER_SYMBOL"
echo "NATIVE_BLOCK_SYMBOL=$NATIVE_BLOCK_SYMBOL"
echo "QUEUED_TASK_SYMBOL=$QUEUED_TASK_SYMBOL"
echo "SPAWN_ACTIVE_SYMBOL=$SPAWN_ACTIVE_SYMBOL"
echo "BLOCKER_SYMBOL=$BLOCKER_SYMBOL"
ldd "$OUT/elf_unload_quiescence"
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$CANGJIE_SDK/runtime/lib/linux_x86_64_cjnative${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    ldd "$OUT/package_snapshot_public"

set +e
aggregate_rc=0
for mode in gc queued native core direct direct_queued direct_active direct_stw early preinit package; do
    mode_gc=1
    if [[ "$mode" == "core" ]]; then
        # Metadata purge checks need deterministic, single-threaded inspection.
        # The separate gc mode exercises the real GC stack-entry reader.
        mode_gc=0
    fi
    cjEnableGC="$mode_gc" \
    LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$CANGJIE_SDK/runtime/lib/linux_x86_64_cjnative${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$OUT/elf_unload_quiescence" "$mode" \
        "$OUT/libelfunloadprobe.so" "$MARKER_SYMBOL" "$NATIVE_BLOCK_SYMBOL" "$QUEUED_TASK_SYMBOL" \
        "$SPAWN_ACTIVE_SYMBOL" "elfunloadprobe:UnloadProbeType" \
        "$OUT/libelfunloadqueueblocker.so" "$BLOCKER_SYMBOL"
    mode_rc=$?
    echo "MODE=$mode RC=$mode_rc"
    aggregate_rc=$((aggregate_rc | mode_rc))
done
cjEnableGC=1 \
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$CANGJIE_SDK/runtime/lib/linux_x86_64_cjnative${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$OUT/package_snapshot_public" \
    "$OUT/libsnapshot_leaf_r.so" "$OUT/libsnapshot_leaf_t.so" "$OUT/libsnapshot_branch.so" \
    "$OUT/libsnapshot_sibling.so" "$OUT/libsnapshot_root.so"
snapshot_rc=$?
echo "MODE=package_snapshot_public RC=$snapshot_rc"
aggregate_rc=$((aggregate_rc | snapshot_rc))
set -e
exit "$aggregate_rc"
