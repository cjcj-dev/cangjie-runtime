#!/usr/bin/env bash
# Public-entry active-frame rejection, using the existing native-call fixture.
set -euo pipefail
ulimit -c 0
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${ELF_UNLOAD_OUT:?set ELF_UNLOAD_OUT}"
LIB="${ELF_UNLOAD_RUNTIME_LIB_DIR:?set ELF_UNLOAD_RUNTIME_LIB_DIR}"
SDK="${CANGJIE_HOME:?set CANGJIE_HOME}"
HOST="${GC_UNIT_CJC_RUNTIME_LIB_DIR:?set GC_UNIT_CJC_RUNTIME_LIB_DIR}"
mkdir -p "$OUT"
LD_LIBRARY_PATH="$HOST:$SDK/tools/lib:$SDK/third_party/llvm/lib" \
    "${CJC:-$SDK/bin/cjc}" "$ROOT/runtime/tests/elf_unload_quiescence_plugin.cj" \
    -O0 --static-std --output-type=dylib -o "$OUT/libelfunloadprobe.so"
"${CXX:-clang++}" -std=gnu++17 -O0 -g -pthread \
    -I"$ROOT/runtime/src" -I"$ROOT/runtime/include" \
    "$ROOT/runtime/tests/elf_unload_public_active.cpp" \
    -L"$LIB" -Wl,-rpath,"$LIB" -Wl,--export-dynamic \
    -lcangjie-runtime -lboundscheck -ldl -o "$OUT/elf_unload_public_active"
sha256sum "$OUT/elf_unload_public_active" "$OUT/libelfunloadprobe.so" \
    "$LIB/libcangjie-runtime.so" "$LIB/libboundscheck.so" > "$OUT/identity.txt"
LD_LIBRARY_PATH="$LIB:$SDK/runtime/lib/linux_x86_64_cjnative" \
    "$OUT/elf_unload_public_active" "$OUT/libelfunloadprobe.so" \
    _CN14elfunloadprobe12unloadMarkerHv _CN14elfunloadprobe17unloadSpawnActiveHPu
LD_LIBRARY_PATH="$LIB:$SDK/runtime/lib/linux_x86_64_cjnative" \
    "$OUT/elf_unload_public_active" "$OUT/libelfunloadprobe.so" \
    _CN14elfunloadprobe12unloadMarkerHv _CN14elfunloadprobe17unloadSpawnParkedHPu parked
