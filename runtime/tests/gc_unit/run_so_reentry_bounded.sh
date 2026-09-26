#!/usr/bin/env bash
# Bounded mutator stack-overflow recovery: the guard expansion the re-entrant recovery
# cycle performs must be a state transition, not one arithmetic step per turn.
# HotSpot counterparts: runtime/stackOverflow.hpp:41-45 (the guard state) and
# runtime/stackOverflow.cpp:220-223 (reguard_stack returns early when the state is
# already in the target state).
#
# Real entry: the probe calls the product's own CangjieRuntime::CreateSingleThreadScheduler
# (CangjieRuntime.cpp:230), which binds the calling thread to a cjthread with an allocated
# stack of its own — the shape the cangjie-runtime#1167 core was taken on. The probe links
# the product SO, compiles no product source and adds no product export; the observed value
# is the product's own stack guard, read back through the product getter.
set -euo pipefail
ulimit -c 0

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
: "${GCV2_RUNTIME_LIB_DIR:?set the product library directory}"
: "${SIGNAL_TEST_OUTPUT:?set a persistent directory for the executable and logs}"
CXX="${CXX:-clang++}"

mkdir -p "$SIGNAL_TEST_OUTPUT"
"$CXX" -std=c++17 -O2 -pthread -fno-rtti \
    -I"$repo/runtime/src" -I"$repo/runtime/src/Heap" -I"$repo/runtime/include" \
    -I"$repo/runtime/third_party/third_party_bounds_checking_function/include" \
    "$repo/runtime/tests/gc_unit/so_reentry_probe.cpp" \
    -L"$GCV2_RUNTIME_LIB_DIR" -Wl,-rpath,"$GCV2_RUNTIME_LIB_DIR" \
    -lcangjie-runtime -lboundscheck -ldl -o "$SIGNAL_TEST_OUTPUT/so-reentry-bounded"
export LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Hashes captured where they are produced, before any run.
sha256sum "$SIGNAL_TEST_OUTPUT/so-reentry-bounded" \
    "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" \
    >"$SIGNAL_TEST_OUTPUT/sha256.txt"

# One process per case: each case owns one target assertion, so a red arm can neither be
# masked by nor mask another case's assertion.
declare -A TARGET=(
  [once]=SO_REENTRY_EXPAND_ONCE_OK
  [bounded]=SO_REENTRY_EXPAND_BOUNDED_OK
  [recover]=SO_REENTRY_EXPAND_RECOVER_OK
  [reentry]=SO_REENTRY_EXPAND_REENTRY_OK
)
overall=0
for case_name in once bounded recover reentry; do
  log="$SIGNAL_TEST_OUTPUT/so_reentry_$case_name.log"
  set +e
  "$SIGNAL_TEST_OUTPUT/so-reentry-bounded" "$case_name" >"$log" 2>&1
  rc=$?
  set -e
  echo "$rc" >"$SIGNAL_TEST_OUTPUT/so_reentry_$case_name.rc"
  cat "$log"
  marker="${TARGET[$case_name]}"
  # The target assertion must have run and reported, in every arm, pass or fail.
  if ! /usr/bin/grep -q "$marker" "$log"; then
    echo "SO_REENTRY_BOUNDED_FAIL case=$case_name rc=$rc target_assertion=$marker not_reported" >&2
    overall=1
    continue
  fi
  if [[ "$rc" != 0 ]]; then
    echo "SO_REENTRY_BOUNDED_FAIL case=$case_name rc=$rc target_assertion=$marker" >&2
    overall=1
  fi
done

if [[ $overall -ne 0 ]]; then
  exit 1
fi
echo "SO_REENTRY_BOUNDED_OK cases=4 target_assertions=4"
