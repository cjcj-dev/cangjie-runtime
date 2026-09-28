#!/usr/bin/env bash
# Acceptance for cangjie-runtime#1253. All arms share one test ELF (built by
# run_signal_fatal_watermark.sh); the product SO is selected per arm through
# LD_LIBRARY_PATH.
#   arm watermark: SIGABRT delivered to a thread holding the StackWatermark
#       mutex must print the fatal diagnostics and terminate the inferior,
#       never self-lock. Termination is proven by gdb's own
#       "Program terminated with signal" report plus pid death. An inferior
#       surviving FATAL_POST_WAIT is a hang: the injecting gdb is SIGKILLed
#       (the tracee resumes, still hung) and a fresh gdb attach captures the
#       blocked thread's stack and the mutex owner (same-thread reentry).
#   arm normal: a non-fatal signal (SIGUSR1) must still reach the registered
#       user handler (non-regression of the managed dispatch path).
# Env: SIGNAL_TEST_OUTPUT (dir with the built ELF), ARM_LIB_DIR (product SO
#      dir of this arm), ARM_NAME, FATAL_TIMEOUT (s, def 90),
#      FATAL_POST_WAIT (s after injection before declaring a hang, def 15).
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

pid_state() { # prints R/S/t/Z... or empty when gone
    [ -n "$1" ] && [ -r "/proc/$1/stat" ] || return 1
    sed -n 's/.*) \([A-Za-z]\) .*/\1/p' "/proc/$1/stat"
}

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
verdict=''
ipid=''
gdb_rc=''
while :; do
    if [ -z "$verdict" ] && /usr/bin/grep -q 'FATAL_INJECTING' "$log"; then
        ipid=$(/usr/bin/grep -oP 'INFERIOR_PID=\K[0-9]+' "$log" | tail -1)
        # post_wait grace: a correct fatal path terminates within this window.
        settle_deadline=$((SECONDS + post_wait))
        while [ $SECONDS -lt $settle_deadline ]; do
            state=$(pid_state "$ipid" || true)
            [ -z "$state" ] || [ "$state" = "Z" ] && { verdict=terminated; break; }
            kill -0 "$gdb_pid" 2>/dev/null || { verdict=terminated; break; }
            sleep 1
        done
        [ -z "$verdict" ] && verdict=survived
    fi
    if [ "$verdict" = "survived" ]; then
        echo "inferior $ipid survived ${post_wait}s after FATAL injection; interrupting gdb for stack capture"
        # SIGINT to gdb interrupts its `signal` wait exactly like an
        # interactive Ctrl-C: the inferior stops and the gdb script captures
        # the blocked stack and the mutex owner (WATERMARK_SELFLOCK).
        kill -INT "$gdb_pid" 2>/dev/null
        wait "$gdb_pid" 2>/dev/null
        gdb_rc=$?
        [ -n "$ipid" ] && kill -KILL "$ipid" 2>/dev/null
        verdict=hang
    fi
    if [ "$verdict" = "terminated" ]; then
        wait "$gdb_pid" 2>/dev/null
        gdb_rc=$?
    fi
    [ -n "$verdict" ] && break
    if ! kill -0 "$gdb_pid" 2>/dev/null; then
        # gdb exited before any injection marker: injection setup failed or
        # the process died early; decide from the log below.
        ipid=$(/usr/bin/grep -oP 'INFERIOR_PID=\K[0-9]+' "$log" | tail -1)
        verdict=early-exit
        break
    fi
    if [ $SECONDS -ge $deadline ]; then
        echo "WATERMARK_ARM_TIMEOUT killing gdb/inferior"
        ipid=$(/usr/bin/grep -oP 'INFERIOR_PID=\K[0-9]+' "$log" | tail -1)
        kill -KILL "$gdb_pid" 2>/dev/null
        [ -n "$ipid" ] && kill -KILL "$ipid" 2>/dev/null
        verdict=timeout
        break
    fi
    sleep 1
done
if [ -z "$gdb_rc" ]; then
    wait "$gdb_pid" 2>/dev/null
    gdb_rc=$?
fi
echo "GDB_RC=$gdb_rc" | tee "$arm_dir/watermark.rc"
inferior_gone=0
if [ -n "$ipid" ] && [ -z "$(pid_state "$ipid" || true)" ]; then inferior_gone=1; fi
echo "INFERIOR_GONE=$inferior_gone VERDICT=$verdict"

if /usr/bin/grep -q 'GDB_ERROR' "$log"; then
    echo "WATERMARK_ARM_GDB_ERROR (debugger failure, not a product verdict)"
    rc_all=1
elif [ "$verdict" = "terminated" ]; then
    if ! /usr/bin/grep -q 'INFERIOR_TERMINATED' "$log"; then
        echo "WATERMARK_ARM_NO_TERM_MARKER"
        rc_all=1
    elif ! /usr/bin/grep -q 'Program terminated with signal SIGABRT' "$log"; then
        echo "WATERMARK_ARM_NO_SIGNAL_TERM (gdb did not observe SIGABRT termination)"
        rc_all=1
    elif ! /usr/bin/grep -q 'CJNative Handle signal: 6' "$log" || ! /usr/bin/grep -q 'signal SIGABRT (6)' "$log"; then
        echo "WATERMARK_ARM_NOFATAL (fatal diagnostics missing)"
        rc_all=1
    elif ! /usr/bin/grep -q 'WATERMARK_LOCK_TARGET .*held=1' "$log"; then
        echo "WATERMARK_ARM_NOLOCK (lock ownership not verified)"
        rc_all=1
    elif [ $inferior_gone -ne 1 ]; then
        echo "WATERMARK_ARM_STILL_ALIVE"
        rc_all=1
    else
        echo "WATERMARK_ARM_PASS gdb_rc=$gdb_rc (fatal diagnostics emitted; inferior terminated by SIGABRT)"
    fi
elif [ "$verdict" = "hang" ]; then
    if /usr/bin/grep -q 'WATERMARK_SELFLOCK .*same=1' "$log" && [ "$gdb_rc" -eq 3 ]; then
        echo "WATERMARK_ARM_HANG gdb_rc=$gdb_rc (same-thread reentry self-lock reproduced)"
        /usr/bin/grep '^POSTINJECT|' "$log" | head -30
    else
        echo "WATERMARK_ARM_HANG_UNPROVEN (survived but self-lock not captured)"
    fi
    rc_all=1
else
    echo "WATERMARK_ARM_UNKNOWN verdict=$verdict gdb_rc=$gdb_rc"
    rc_all=1
fi
exit $rc_all
