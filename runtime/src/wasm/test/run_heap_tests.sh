#!/bin/bash
# Builds and runs the wasm32-wasi heap/GC unit tests under node:wasi.
#   run_heap_tests.sh <clang> <wasm-ld> <wasi-sysroot> <run_wasi.mjs> <outdir> [heap.c override]
# Exit status = number of failed expectations (0 = all met). Prints HEAP_TEST_FAILS=<n>.
set -u
CC=$1; LD=$2; SR=$3; RUN=$4; OUT=$5
NODE=${NODE:-node}
# V8 crashes in a GC triggered from the WASI fd_write fast-API callback once the linear memory is large
# (InnerPointerToCodeCache::GetCacheEntry, node v20.19.0 and v22.23.3); disable fast API calls.
NODE_FLAGS=${NODE_FLAGS:---no-turbo-fast-api-calls}
HERE=$(cd "$(dirname "$0")" && pwd); SRC=$(dirname "$HERE")
HEAPC=${6:-$SRC/CjWasmHeap.c}
mkdir -p "$OUT"; cd "$OUT" || exit 99
FLAGS="--target=wasm32-wasi --sysroot=$SR -O2 -std=c11 -Wall -Wextra -Werror -fno-exceptions -I$SRC"
FAILS=0
"$CC" $FLAGS -c "$HEAPC" -o heap.o; c1=$?
"$CC" $FLAGS -c "$HERE/CjWasmHeapTest.c" -o heap_test.o; c2=$?
"$LD" -o heap_test.wasm "$SR/lib/wasm32-wasi/crt1-command.o" heap.o heap_test.o -L"$SR/lib/wasm32-wasi" -lc; c3=$?
echo "build cc_heap=$c1 cc_test=$c2 ld=$c3"
if [ $c1 -ne 0 ] || [ $c2 -ne 0 ] || [ $c3 -ne 0 ]; then echo "HEAP_TEST_FAILS=99"; exit 99; fi
sha256sum heap_test.wasm; echo "node=$("$NODE" --version) flags=$NODE_FLAGS"
"$NODE" $NODE_FLAGS "$RUN" heap_test.wasm all > all.out 2> all.err; r=$?
cat all.out
grep -q '^HEAP_UNIT_FAILS=0$' all.out && [ $r -eq 0 ] || FAILS=$((FAILS+1))
echo "mode all rc=$r"
# mode expected_rc expected_stderr_fragment
while read -r mode want frag; do
  "$NODE" $NODE_FLAGS "$RUN" heap_test.wasm "$mode" > "$mode.out" 2> "$mode.err"; r=$?
  msg=$(grep -v -i 'experimentalwarning\|trace-warnings' "$mode.err" | head -c 220 | tr '\n' '|')
  v=FAIL; if [ "$r" = "$want" ] && { [ "$frag" = - ] || grep -q -- "$frag" "$mode.err"; }; then v=PASS; fi
  [ $v = PASS ] || FAILS=$((FAILS+1))
  echo "$v mode=$mode rc=$r want=$want out=$(tr '\n' '|' < "$mode.out") err=$msg"
done <<'MODES'
gate 0 -
collect_before_enable 0 -
no_gctib 135 no GC tib
rt_varray 135 no derivable runtime field layout
wild_ti 139 outside linear memory
tib_beyond 139 beyond its
array_bounds 139 exceeds its
frame_order 139 is not below its caller frame
table_in_heap 139 is NULL or inside the managed heap
verify_interior 139 not at the start of a live object
MODES
# Runtime-held roots: full runtime (CjWasmRuntime.c + heap) with a test-supplied Cangjie entry.
RTC=${7:-$SRC/CjWasmRuntime.c}
"$CC" $FLAGS -c "$RTC" -o rt.o && "$CC" $FLAGS -c "$HERE/CjWasmRuntimeRootTest.c" -o rt_root_test.o && \
  "$LD" -o rt_root_test.wasm "$SR/lib/wasm32-wasi/crt1-command.o" rt.o heap.o rt_root_test.o -L"$SR/lib/wasm32-wasi" -lc; c4=$?
r=NA; [ $c4 -eq 0 ] && { "$NODE" $NODE_FLAGS "$RUN" rt_root_test.wasm > rt_root.out 2> rt_root.err; r=$?; }
v=FAIL; [ "$r" = 0 ] && grep -q '^THREAD_ROOT_FAILS=0$' rt_root.out && v=PASS
[ $v = PASS ] || FAILS=$((FAILS+1))
echo "$v mode=rt_thread_root rc=$r want=0 build=$c4 out=$(tr '\n' '|' < rt_root.out 2>/dev/null)"
echo "HEAP_TEST_FAILS=$FAILS"
exit $FAILS
