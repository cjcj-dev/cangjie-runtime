#!/usr/bin/env bash
# A1/B1 targeted matrix. All product arms use the same already linked ELF.
set -uo pipefail
ulimit -c 0
: "${SIGNAL_TEST_OUTPUT:?}" "${ARM_LIB_DIR:?}" "${ARM_NAME:?}"
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
out="$SIGNAL_TEST_OUTPUT/platform-$ARM_NAME"
mkdir -p "$out"
bin="$SIGNAL_TEST_OUTPUT/signal-fatal-watermark"
export LD_LIBRARY_PATH="$ARM_LIB_DIR"
sha256sum "$bin" "$ARM_LIB_DIR"/*.so > "$out/identity.sha256"
ldd "$bin" > "$out/ldd.txt"
uptime > "$out/uptime-before.txt"
rc_all=0
record() {
    echo "TARGET name=$1 executed=1 pass=$2 process_rc=$3" | tee -a "$out/results.txt"
    [ "$2" = 1 ] || rc_all=1
}
for sig in 10 13 25; do
    for mode in native native-info ignored; do
        timeout --signal=KILL 10 "$bin" "$mode" "$sig" > "$out/$mode-$sig.log" 2>&1
        rc=$?; ok=1; [ "$rc" = 0 ] || ok=0
        managed=0; result=$sig; reset=1
        if [ "$sig" = 10 ]; then managed=10; result=0; reset=0; fi
        if [ "$mode" = ignored ]; then pattern="SIGNAL_IGNORE_TARGET executed=1 managed=$managed"
        else pattern="SIGNAL_NATIVE_TARGET executed=1 result=$result reset=$reset managed=$managed"; fi
        /usr/bin/grep -q "$pattern" "$out/$mode-$sig.log" || ok=0
        record "$mode-$sig" "$ok" "$rc"
    done
done
for sig in 4 5 6 7 8 11; do
    timeout --signal=KILL 10 "$bin" unhandled "$sig" > "$out/diagnostic-$sig.log" 2>&1
    rc=$?; ok=1; [ "$rc" = "$((128+sig))" ] || ok=0
    /usr/bin/grep -Eq "signal SIG[A-Z]+ \($sig\) pc=" "$out/diagnostic-$sig.log" || ok=0
    record "diagnostic-$sig" "$ok" "$rc"
done
timeout --signal=KILL 10 "$bin" guard 11 > "$out/guard.log" 2>&1
rc=$?; ok=1; [ "$rc" = 139 ] || ok=0
/usr/bin/grep -q 'SIGNAL_GUARD_INPUT.*in_guard=1' "$out/guard.log" || ok=0
/usr/bin/grep -q 'unhandled SIGSEGV from unmanaged stack overflow!' "$out/guard.log" || ok=0
record guard "$ok" "$rc"
timeout --signal=KILL 30 gdb -q -batch -x "$repo/runtime/tests/signal_exception_gdb.py" --args "$bin" exception 6 > "$out/exception.log" 2>&1
rc=$?; ok=1; [ "$rc" = 0 ] || ok=0
for pattern in 'EXCEPTION_PRODUCER_TARGET pending_nonnull=1' 'Two pending exceptions are thrown when a C function calls Cangjie function. Program will abort.' 'EXCEPTION_EXIT_TARGET terminated=1' 'Program terminated with signal SIGABRT'; do
    /usr/bin/grep -Fq "$pattern" "$out/exception.log" || ok=0
done
record exception "$ok" "$rc"
uptime > "$out/uptime-after.txt"
exit "$rc_all"
