#!/usr/bin/env bash
# Exercise the actual parallel runner, including an exec'd VM and another
# live thread at the signal stop. Only the selected case may turn red.
set -euo pipefail
ulimit -c 0
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: test_capture_segv.sh ARTIFACT_DIR}
mkdir -p "$OUT"
cat >"$OUT/control.cpp" <<'CPP'
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

__attribute__((noinline)) void CaptureBystander() { pause(); }
__attribute__((noinline)) void TriggerSegv() { raise(SIGSEGV); }
int main(int argc, char** argv) {
    if (argc == 2 && (!strcmp(argv[1], "--child") || !strcmp(argv[1], "--grandchild"))) {
        if (!strcmp(argv[1], "--child")) {
            pid_t child = fork();
            if (child == 0) { execl(argv[0], argv[0], "--grandchild", nullptr); _exit(127); }
            int status = 0;
            if (child < 0 || waitpid(child, &status, 0) != child) return 98;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 7;
        }
        if (getenv("CAPTURE_CONTROL_RAISE")) {
            std::thread sleeper(CaptureBystander);
            // Wait for the named bystander to be live before raising.
            usleep(100000);
            TriggerSegv();
            return 99;
        }
        return 0;
    }
    bool publication = strstr(argv[0], "publication") != nullptr;
    const char* name = publication ? "CaptureControl.Unrelated" : "ZVerifyCarrier.ArmedBadRootIsSkipped";
    if (argc == 2 && !strcmp(argv[1], "--gtest_list_tests")) {
        puts(publication ? "CaptureControl.\n  Unrelated" : "ZVerifyCarrier.\n  ArmedBadRootIsSkipped");
        return 0;
    }
    printf("[  RUN   ] %s\n", name);
    fflush(stdout);
    bool failed = false;
    if ((!publication && getenv("CAPTURE_CONTROL_NESTED")) || getenv("CAPTURE_CONTROL_RAISE")) {
        pid_t child = fork();
        if (child == 0) { execl(argv[0], argv[0], "--child", nullptr); _exit(127); }
        int status = 0;
        if (child < 0 || waitpid(child, &status, 0) != child) return 98;
        failed = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    }
    printf("[  %s  ] %s\n", failed ? "FAIL" : "PASS", name);
    if (const char* tally = getenv("GC_UNIT_TALLY_FILE")) {
        FILE* f = fopen(tally, "w");
        if (!f) return 97;
        fprintf(f, "[========] 1 tests: %d passed, %d failed\n", !failed, failed);
        fclose(f);
    }
    return failed ? 7 : 0;
}
CPP
if [[ $# -eq 2 ]]; then
  # Cut/recovery runs reuse the exact same already-built native control pair.
  cp "$2/main" "$OUT/main"
  cp "$2/publication" "$OUT/publication"
else
  "${CXX:-clang++}" -O0 -g -pthread "$OUT/control.cpp" -o "$OUT/main"
  cp "$OUT/main" "$OUT/publication"
fi
sha256sum "$OUT/main" "$OUT/publication" >"$OUT/elf.sha256"
run_arm() {
  local arm=$1 rc
  local -a control_env=(-u CAPTURE_CONTROL_RAISE -u CAPTURE_CONTROL_NESTED)
  if [[ "$arm" == signal ]]; then control_env=(CAPTURE_CONTROL_RAISE=1 CAPTURE_CONTROL_NESTED=1); fi
  if [[ "$arm" == nested ]]; then control_env=(-u CAPTURE_CONTROL_RAISE CAPTURE_CONTROL_NESTED=1); fi
  set +e
  env "${control_env[@]}" GC_UNIT_JOBS=$(nproc) \
    bash "$SRC/run_parallel_tests.sh" "$OUT/main" "$OUT/publication" "$OUT/$arm" "$OUT" \
    >"$OUT/$arm.log" 2>&1
  rc=$?
  set -e
  printf '%s\n' "$rc" >"$OUT/$arm.rc"
}
# Independent arm directories share only the immutable ELF pair.
run_arm green &
green_pid=$!
run_arm signal &
signal_pid=$!
run_arm restored &
restored_pid=$!
run_arm nested &
nested_pid=$!
wait "$green_pid" "$signal_pid" "$restored_pid" "$nested_pid"
for arm in green restored nested; do
  [[ $(cat "$OUT/$arm.rc") -eq 0 ]]
  /usr/bin/grep -qxF '[========] 2 tests: 2 passed, 0 failed' "$OUT/$arm/parallel_tally.txt"
done
[[ $(cat "$OUT/signal.rc") -eq 1 ]]
/usr/bin/grep -qxF '[========] 2 tests: 0 passed, 2 failed' "$OUT/signal/parallel_tally.txt"
[[ $(cat "$OUT/signal/test-rc/000000-main.rc") -eq 7 ]]
[[ $(cat "$OUT/signal/test-rc/000001-publication.rc") -eq 7 ]]
! /usr/bin/grep -qF 'GC_UNIT_SIGSEGV_CAPTURE_BEGIN' "$OUT/signal/test-logs/000001-publication.log"
echo 'CAPTURE_STATUS_ASSERTIONS_OK target_rc=7 unrelated_rc=7 nested_rc=0'
for arm in green restored nested; do
  /usr/bin/grep -qF 'GC_UNIT_SIGSEGV_CAPTURE_READY' "$OUT/$arm.log"
done
/usr/bin/grep -qF 'GC_UNIT_SIGSEGV_CAPTURE_BEGIN' "$OUT/signal.log"
# Check only the all-thread section; the separate fault bt cannot satisfy it.
sed -n '/GC_UNIT_SIGSEGV_COMMAND thread apply all bt full/,/GC_UNIT_SIGSEGV_COMMAND bt full/p' "$OUT/signal.log" >"$OUT/all-thread.log"
/usr/bin/grep -qF 'CaptureBystander' "$OUT/all-thread.log"
echo 'CAPTURE_ALL_THREADS_ASSERTION_OK'
/usr/bin/grep -qF 'TriggerSegv' "$OUT/signal.log"
/usr/bin/grep -qF 'GC_UNIT_SIGSEGV_CAPTURE_END' "$OUT/signal.log"
/usr/bin/grep -qF 'si_signo = 11' "$OUT/signal.log"
[[ $(cat "$OUT/signal/test-rc/000000-main.rc") -eq 7 ]]
/usr/bin/grep -qF 'GC_UNIT_SIGSEGV_MAPS_BEGIN' "$OUT/signal.log"
/usr/bin/grep -qF "sha256=$(cut -d' ' -f1 "$OUT/elf.sha256" | head -1)" "$OUT/signal.log"
/usr/bin/grep -qF 'GC_UNIT_SIGSEGV_COMMAND info registers' "$OUT/signal.log"
/usr/bin/grep -qF 'status=COMPLETE' "$OUT/signal.log"
/usr/bin/grep -qF 'captured=yes' "$OUT/signal.log"
python3 - "$OUT/signal.log" <<'PYCONTROL'
import re, sys
text = open(sys.argv[1]).read()
match = re.search(r'CAPTURE_BEGIN inferior=(\d+) pid=(\d+) tid=([^\n]+)', text)
assert match, 'fault PID/TID missing'
inferior, pid, tid = match.groups()
assert int(inferior) >= 3, 'signal must be in the second fork/exec descendant'
assert 'GC_UNIT_SIGSEGV_MAPS_BEGIN pid=' + pid in text, 'maps belong to fault PID'
assert 'GC_UNIT_SIGSEGV_CAPTURE_STATUS inferior=' + inferior + ' status=COMPLETE' in text
assert 'GC_UNIT_SIGSEGV_INFERIOR_EXIT inferior=' + inferior in text
assert text.count('GC_UNIT_SIGSEGV_INFERIOR_EXIT ') == 3, 'root and both descendants drained'
assert 'GC_UNIT_SIGSEGV_ROOT_STATUS exit_code=7 captured_inferiors=1' in text
assert re.search(r'^rip\s+0x', text, re.M), 'stopped fault registers missing'
print('CAPTURE_IDENTITY_ASSERTIONS_OK fault_inferior=' + inferior + ' pid=' + pid)
PYCONTROL
echo 'CAPTURE_SEGV_CONTROL_OK green=0 signal=1 restored=0 target_rc=7 unrelated_rc=7'
