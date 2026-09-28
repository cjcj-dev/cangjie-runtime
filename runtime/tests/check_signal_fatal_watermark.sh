#!/usr/bin/env bash
# Acceptance for cangjie-runtime#1253. All arms share one test ELF (built by
# run_signal_fatal_watermark.sh); the product SO is selected per arm through
# LD_LIBRARY_PATH.
#   arm watermark: SIGABRT(FATAL) while this thread owns the StackWatermark
#       mutex must print the FATAL text and terminate the inferior, never
#       self-lock. A surviving inferior is SIGINT-stopped before any timeout
#       cleanup so its post-injection stack and the mutex owner are captured.
#   arm normal: a non-fatal signal (SIGUSR1) must still reach the registered
#       user handler (non-regression of the managed dispatch path).
# Env: SIGNAL_TEST_OUTPUT (dir with the built ELF), ARM_LIB_DIR (product SO
#      dir of this arm), ARM_NAME, FATAL_TIMEOUT (s, def 90),
#      FATAL_POST_WAIT (s between lock-verified injection and the SIGINT
#      capture of a surviving inferior, def 15).
set -uo pipefail
ulimit -c 0
: "${SIGNAL_TEST_OUTPUT:?}" "${ARM_LIB_DIR:?}" "${ARM_NAME:?}"
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
srcbin="$SIGNAL_TEST_OUTPUT/signal-fatal-watermark"
arm_dir="$SIGNAL_TEST_OUTPUT/arm-$ARM_NAME"
mkdir -p "$arm_dir"
bin="$arm_dir/signal-fatal-watermark"
cp "$srcbin" "$bin"
timeout_s=${FATAL_TIMEOUT:-90}
post_wait=${FATAL_POST_WAIT:-15}

# Identity actually used by this arm: the copied single ELF, the selected SOs,
# and the loader resolution with this arm's LD_LIBRARY_PATH.
sha256sum "$bin" "$ARM_LIB_DIR"/libcangjie-runtime.so "$ARM_LIB_DIR"/libboundscheck.so \
    "$ARM_LIB_DIR"/libcangjie-trace.so > "$arm_dir/identity.sha256"
LD_LIBRARY_PATH="$ARM_LIB_DIR" ldd "$bin" | /usr/bin/grep -E 'cangjie|boundscheck' > "$arm_dir/ldd.txt"
cat "$arm_dir/identity.sha256" "$arm_dir/ldd.txt"

rc_all=0

echo "== arm[$ARM_NAME]: normal signal (SIGUSR1) non-regression =="
LD_LIBRARY_PATH="$ARM_LIB_DIR" timeout --signal=KILL 60 "$bin" normal 10 > "$arm_dir/normal.log" 2>&1
rc=$?
echo "NORMAL_RC=$rc" | tee "$arm_dir/normal.rc"
cat "$arm_dir/normal.log"
if [ $rc -ne 0 ] || ! /usr/bin/grep -q 'SIGNAL_NORMAL_TARGET executed=1 signal=10 observed=10' "$arm_dir/normal.log"; then
    echo "NORMAL_ARM_FAIL rc=$rc"
    rc_all=1
else
    echo "NORMAL_ARM_PASS"
fi

echo "== arm[$ARM_NAME]: FATAL while owning StackWatermark lock (gdb injection) =="
log="$arm_dir/watermark.log"
LD_LIBRARY_PATH="$ARM_LIB_DIR" gdb -q -batch \
    -x "$repo/runtime/tests/signal_fatal_watermark_gdb.py" \
    --args "$bin" watermark 6 > "$log" 2>&1 &
gdb_pid=$!
deadline=$((SECONDS + timeout_s))
interrupted=0
while kill -0 "$gdb_pid" 2>/dev/null; do
    if [ $interrupted -eq 0 ] && /usr/bin/grep -q 'WATERMARK_LOCK_TARGET .*held=1' "$log"; then
        sleep "$post_wait"
        interrupted=1
        ipid=$(/usr/bin/grep -oP 'INFERIOR_PID=\K[0-9]+' "$log" | tail -1)
        if [ -n "$ipid" ] && kill -0 "$ipid" 2>/dev/null; then
            echo "inferior $ipid survived ${post_wait}s after FATAL injection; SIGINT stop for stack capture"
            kill -INT "$ipid"
        fi
    fi
    if [ $SECONDS -ge $deadline ]; then
        echo "WATERMARK_ARM_TIMEOUT killing gdb/inferior"
        ipid=$(/usr/bin/grep -oP 'INFERIOR_PID=\K[0-9]+' "$log" | tail -1)
        [ -n "$ipid" ] && kill -KILL "$ipid" 2>/dev/null
        kill -KILL "$gdb_pid" 2>/dev/null
        break
    fi
    sleep 1
done
wait "$gdb_pid"
gdb_rc=$?
echo "GDB_RC=$gdb_rc" | tee "$arm_dir/watermark.rc"
ipid=$(/usr/bin/grep -oP 'INFERIOR_PID=\K[0-9]+' "$log" | tail -1 || true)
inferior_gone=0
if [ -n "$ipid" ] && ! kill -0 "$ipid" 2>/dev/null; then inferior_gone=1; fi
echo "INFERIOR_GONE=$inferior_gone"

if /usr/bin/grep -q 'GDB_ERROR' "$log"; then
    echo "WATERMARK_ARM_GDB_ERROR (debugger failure, not a product verdict)"
    rc_all=1
elif /usr/bin/grep -q 'INFERIOR_TERMINATED' "$log" && [ $gdb_rc -eq 0 ]; then
    if ! /usr/bin/grep -q 'FATAL_WATERMARK_1253' "$log"; then
        echo "WATERMARK_ARM_NOFATAL"
        rc_all=1
    elif ! /usr/bin/grep -q 'WATERMARK_LOCK_TARGET .*held=1' "$log"; then
        echo "WATERMARK_ARM_NOLOCK (lock ownership not verified)"
        rc_all=1
    elif [ $inferior_gone -ne 1 ]; then
        echo "WATERMARK_ARM_STILL_ALIVE"
        rc_all=1
    else
        echo "WATERMARK_ARM_PASS gdb_rc=$gdb_rc (FATAL text emitted; inferior process terminated)"
    fi
elif /usr/bin/grep -q 'WATERMARK_SELFLOCK .*same=1' "$log" && [ $gdb_rc -eq 3 ]; then
    echo "WATERMARK_ARM_HANG gdb_rc=$gdb_rc (same-thread reentry self-lock reproduced)"
    /usr/bin/grep '^POSTINJECT|' "$log" | head -30
    rc_all=1
else
    echo "WATERMARK_ARM_UNKNOWN gdb_rc=$gdb_rc"
    rc_all=1
fi
exit $rc_all
