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

__attribute__((noinline)) void TriggerSegv() { raise(SIGSEGV); }
int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--child")) {
        std::thread sleeper([] { pause(); });
        TriggerSegv();
        return 99;
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
    if (!publication && getenv("CAPTURE_CONTROL_RAISE")) {
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
"${CXX:-clang++}" -O0 -g -pthread "$OUT/control.cpp" -o "$OUT/main"
cp "$OUT/main" "$OUT/publication"
sha256sum "$OUT/main" "$OUT/publication" >"$OUT/elf.sha256"
run_arm() {
  local arm=$1 rc
  set +e
  GC_UNIT_JOBS=$(nproc) CAPTURE_CONTROL_RAISE=${2:-} \
    bash "$SRC/run_parallel_tests.sh" "$OUT/main" "$OUT/publication" "$OUT/$arm" "$OUT" \
    >"$OUT/$arm.log" 2>&1
  rc=$?
  set -e
  printf '%s\n' "$rc" >"$OUT/$arm.rc"
}
# Empty env is removed for green arms: the native fixture tests presence.
unset CAPTURE_CONTROL_RAISE
for arm in green restored; do
  set +e
  GC_UNIT_JOBS=$(nproc) bash "$SRC/run_parallel_tests.sh" \
    "$OUT/main" "$OUT/publication" "$OUT/$arm" "$OUT" >"$OUT/$arm.log" 2>&1
  rc=$?
  set -e
  echo "$rc" >"$OUT/$arm.rc"
  [[ "$rc" -eq 0 ]]
  /usr/bin/grep -qxF '[========] 2 tests: 2 passed, 0 failed' "$OUT/$arm/parallel_tally.txt"
  /usr/bin/grep -qF 'GC_UNIT_SIGSEGV_CAPTURE_READY' "$OUT/$arm.log"
done
run_arm signal 1
[[ $(cat "$OUT/signal.rc") -eq 1 ]]
/usr/bin/grep -qxF '[========] 2 tests: 1 passed, 1 failed' "$OUT/signal/parallel_tally.txt"
/usr/bin/grep -qF 'GC_UNIT_SIGSEGV_CAPTURE_BEGIN' "$OUT/signal.log"
/usr/bin/grep -qF 'TriggerSegv' "$OUT/signal.log"
/usr/bin/grep -qF 'GC_UNIT_SIGSEGV_CAPTURE_END' "$OUT/signal.log"
/usr/bin/grep -qF 'si_signo = 11' "$OUT/signal.log"
[[ $(cat "$OUT/signal/test-rc/000000-main.rc") -eq 7 ]]
[[ $(cat "$OUT/signal/test-rc/000001-publication.rc") -eq 0 ]]
echo 'CAPTURE_SEGV_CONTROL_OK green=0 signal=1 restored=0 target_rc=7 unrelated_rc=0'
