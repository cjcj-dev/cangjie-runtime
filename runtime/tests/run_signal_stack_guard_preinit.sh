#!/usr/bin/env bash
# HotSpot counterpart: runtime/StackGuardPages/TestStackGuardPages.java.
set -euo pipefail
ulimit -c 0
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
: "${GCV2_RUNTIME_LIB_DIR:?set the candidate product library directory}"
: "${SIGNAL_TEST_OUTPUT:?set a persistent directory for the executable and logs}"
output_root=${GCV2_RUNTIME_OUTPUT_ROOT:-$(realpath -m "$GCV2_RUNTIME_LIB_DIR/../..")}
mkdir -p "$SIGNAL_TEST_OUTPUT"
clang++ -std=c++17 -O2 -pthread -fno-rtti \
    -I"$repo/runtime/src" -I"$repo/runtime/src/Heap" -I"$repo/runtime/include" \
    -I"$output_root/include" \
    -I"$repo/runtime/third_party/third_party_bounds_checking_function/include" \
    "$repo/runtime/tests/signal_stack_guard_preinit.cpp" \
    -L"$GCV2_RUNTIME_LIB_DIR" -Wl,-rpath,"$GCV2_RUNTIME_LIB_DIR" \
    -lcangjie-runtime -lboundscheck -ldl -o "$SIGNAL_TEST_OUTPUT/signal-stack-guard"
export LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
sha256sum "$SIGNAL_TEST_OUTPUT/signal-stack-guard" \
    "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" \
    > "$SIGNAL_TEST_OUTPUT/sha256.txt"
for test_case in query-no-runtime query-published-runtime no-runtime published-runtime; do
    set +e
    "$SIGNAL_TEST_OUTPUT/signal-stack-guard" "$test_case" >"$SIGNAL_TEST_OUTPUT/$test_case.log" 2>&1
    rc=$?
    set -e
    echo "$rc" >"$SIGNAL_TEST_OUTPUT/$test_case.rc"
    cat "$SIGNAL_TEST_OUTPUT/$test_case.log"
    [[ "$rc" == 0 ]]
    if [[ "$test_case" == query-* ]]; then
        /usr/bin/grep -q 'GUARD_QUERY_RESULT .*in_guard=0 expected=0' "$SIGNAL_TEST_OUTPUT/$test_case.log"
        continue
    fi
    /usr/bin/grep -q 'FAULT_ARMED' "$SIGNAL_TEST_OUTPUT/$test_case.log"
    /usr/bin/grep -Eq 'rec=crash .*signo=11 .*si_code=2 .*pc=0x[1-9a-f]' "$SIGNAL_TEST_OUTPUT/$test_case.log"
done
