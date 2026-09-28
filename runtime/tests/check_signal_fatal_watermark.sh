#!/usr/bin/env bash
# Acceptance for cangjie-runtime#1253.
#   arm1 watermark: SIGABRT(FATAL) while this thread owns the StackWatermark
#       mutex must print the FATAL text and exit, never hang.
#   arm2 normal: a non-fatal signal (SIGUSR1) must still reach the registered
#       user handler (non-regression of the managed dispatch path).
# Env: SIGNAL_TEST_OUTPUT (dir with the built binary), FATAL_TIMEOUT (s, def 60).
set -uo pipefail
ulimit -c 0
: "${SIGNAL_TEST_OUTPUT:?}"
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
bin="$SIGNAL_TEST_OUTPUT/signal-fatal-watermark"
timeout_s=${FATAL_TIMEOUT:-60}
rc_all=0

echo "== arm2: normal signal (SIGUSR1) non-regression =="
"$bin" normal 10 > "$SIGNAL_TEST_OUTPUT/normal.log" 2>&1
rc=$?
cat "$SIGNAL_TEST_OUTPUT/normal.log"
if [ $rc -ne 0 ] || ! /usr/bin/grep -q 'SIGNAL_NORMAL_TARGET executed=1 signal=10 observed=10' "$SIGNAL_TEST_OUTPUT/normal.log"; then
    echo "NORMAL_ARM_FAIL rc=$rc"
    rc_all=1
else
    echo "NORMAL_ARM_PASS"
fi

echo "== arm1: FATAL while owning StackWatermark lock (gdb injection) =="
timeout --signal=KILL "${timeout_s}" gdb -q -batch \
    -x "$repo/runtime/tests/signal_fatal_watermark_gdb.py" \
    --args "$bin" watermark 6 > "$SIGNAL_TEST_OUTPUT/watermark.log" 2>&1
rc=$?
if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
    echo "WATERMARK_ARM_HANG rc=$rc (self-deadlock: FATAL never returned)"
    if /usr/bin/grep -q 'on_safepoint\|start_processing' "$SIGNAL_TEST_OUTPUT/watermark.log"; then
        echo "WATERMARK_ARM_HANG_STACK=watermark"
    fi
    rc_all=1
elif ! /usr/bin/grep -q 'FATAL_WATERMARK_1253' "$SIGNAL_TEST_OUTPUT/watermark.log"; then
    echo "WATERMARK_ARM_NOFATAL rc=$rc"
    rc_all=1
elif ! /usr/bin/grep -q 'WATERMARK_LOCK_TARGET .*held=1' "$SIGNAL_TEST_OUTPUT/watermark.log"; then
    echo "WATERMARK_ARM_NOLOCK (lock ownership not verified)"
    rc_all=1
else
    echo "WATERMARK_ARM_PASS rc=$rc (FATAL text emitted, process exited)"
fi
exit $rc_all
