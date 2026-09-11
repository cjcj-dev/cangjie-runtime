#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
RUNNER="$ROOT/runtime/tests/gc_unit/run_parallel_tests.sh"
TMP=$(mktemp -d "${GC_UNIT_RUNNER_TEST_TMPDIR:-${TMPDIR:-/tmp}}/parallel-runner.XXXXXX")
if [[ -z "${GC_UNIT_RUNNER_TEST_TMPDIR:-}" ]]; then
  trap 'rm -rf "$TMP"' EXIT
else
  echo "PARALLEL_RUNNER_TEST_ARTIFACTS=$TMP"
fi

make_fake() {
  local path=$1 suite=$2 first=$3 second=$4
  sed \
    -e "s/@SUITE@/$suite/g" \
    -e "s/@FIRST@/$first/g" \
    -e "s/@SECOND@/$second/g" \
    "$ROOT/runtime/tests/gc_unit/parallel_runner_fixture.sh.in" >"$path"
  chmod +x "$path"
}

make_fake "$TMP/main" MarkStripe ConcurrentGlobalStealIsLiveAndLossless FailWhenRequested
make_fake "$TMP/publication" PublicationSuite PassTwo PassThree

GC_UNIT_JOBS=2 GC_UNIT_TEST_TIMEOUT=10 GC_UNIT_FAKE_FAIL=MarkStripe.FailWhenRequested \
  GC_UNIT_TALLY_FILE="$TMP/red.tally" \
  bash "$RUNNER" "$TMP/main" "$TMP/publication" "$TMP/red" "$TMP" >"$TMP/red.log" 2>&1 && exit 1
/usr/bin/grep -qxF '[========] 4 tests: 3 passed, 1 failed' "$TMP/red.tally"
/usr/bin/grep -qxF '[  FAILED  ] MarkStripe.FailWhenRequested' "$TMP/red.log"
/usr/bin/grep -qxF 'GC_UNIT_SERIAL tests=1' "$TMP/red.log"
/usr/bin/grep -qxF $'main\tMarkStripe.ConcurrentGlobalStealIsLiveAndLossless\t000000' \
  "$TMP/red/test-manifest.serial.tsv"
[[ $(find "$TMP/red/test-logs" -type f | wc -l) -eq 4 ]]
[[ $(find "$TMP/red/test-rc" -type f | wc -l) -eq 4 ]]

GC_UNIT_JOBS=1 GC_UNIT_TEST_TIMEOUT=10 GC_UNIT_TALLY_FILE="$TMP/green.tally" \
  bash "$RUNNER" "$TMP/main" "$TMP/publication" "$TMP/green" "$TMP" >"$TMP/green.log" 2>&1
/usr/bin/grep -qxF '[========] 4 tests: 4 passed, 0 failed' "$TMP/green.tally"
/usr/bin/grep -qF 'GC_UNIT_PARALLEL jobs=1 tests=4 wall=' "$TMP/green.log"

# A selected process that exits zero without its independent completion tally
# is not a pass. This is the failure mode produced by an inherited list-mode
# environment before the runner cleared and validated it.
GC_UNIT_JOBS=2 GC_UNIT_TEST_TIMEOUT=10 GC_UNIT_FAKE_NO_TALLY=PublicationSuite.PassThree \
  GC_UNIT_TALLY_FILE="$TMP/missing.tally" \
  bash "$RUNNER" "$TMP/main" "$TMP/publication" "$TMP/missing" "$TMP" \
  >"$TMP/missing.log" 2>&1 && exit 1
/usr/bin/grep -qxF '[========] 4 tests: 3 passed, 1 failed' "$TMP/missing.tally"
/usr/bin/grep -qxF '[  FAILED  ] PublicationSuite.PassThree' "$TMP/missing.log"
/usr/bin/grep -qxF 'GC_UNIT_INCOMPLETE tests=1' "$TMP/missing.log"
/usr/bin/grep -qxF '  isolated process incomplete rc=0' \
  "$TMP/missing/test-logs/000003-publication.log"

# A process terminated after its RUN token has no valid end token or tally.
# It must be reported as exactly one incomplete item and keep the total red.
GC_UNIT_JOBS=2 GC_UNIT_TEST_TIMEOUT=10 \
  GC_UNIT_FAKE_KILL=MarkStripe.ConcurrentGlobalStealIsLiveAndLossless \
  GC_UNIT_TALLY_FILE="$TMP/killed.tally" \
  bash "$RUNNER" "$TMP/main" "$TMP/publication" "$TMP/killed" "$TMP" \
  >"$TMP/killed.log" 2>&1 && exit 1
/usr/bin/grep -qxF '[========] 4 tests: 3 passed, 1 failed' "$TMP/killed.tally"
/usr/bin/grep -qxF '[  INCOMPLETE ] MarkStripe.ConcurrentGlobalStealIsLiveAndLossless' "$TMP/killed.log"
/usr/bin/grep -qxF 'GC_UNIT_INCOMPLETE tests=1' "$TMP/killed.log"
/usr/bin/grep -qE '^  isolated process incomplete rc=(137|143)$' \
  "$TMP/killed/test-logs/000000-main.log"
[[ $(find "$TMP/killed/test-logs" -type f | wc -l) -eq 4 ]]
[[ $(find "$TMP/killed/test-rc" -type f | wc -l) -eq 4 ]]

# Ambient list mode may affect discovery, but must be cleared before executing
# each filtered item. All four items therefore still produce completion files.
GC_UNIT_LIST_TESTS=1 GC_UNIT_JOBS=2 GC_UNIT_TEST_TIMEOUT=10 \
  GC_UNIT_TALLY_FILE="$TMP/ambient.tally" \
  bash "$RUNNER" "$TMP/main" "$TMP/publication" "$TMP/ambient" "$TMP" \
  >"$TMP/ambient.log" 2>&1
/usr/bin/grep -qxF '[========] 4 tests: 4 passed, 0 failed' "$TMP/ambient.tally"
/usr/bin/grep -qxF 'GC_UNIT_INCOMPLETE tests=0' "$TMP/ambient.log"
[[ $(find "$TMP/ambient/test-tallies" -type f | wc -l) -eq 4 ]]

# Exercise discovery through the real runner, including its manifest, filters
# and completion accounting. Unlike the older execution fixture, this child
# rejects unknown filters so an invented test cannot accidentally pass.
cat >"$TMP/discovery" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == --gtest_list_tests ]]; then
  case "${DISCOVERY_MODE:-single}" in
    empty) exit 0 ;;
    failure) echo 'listing failed: diagnostic' >&2; exit 7 ;;
    grammar)
      printf '  Orphan\nBad Suite.\n  Lost\nBad:Suite.\n  LostAgain\n'
      printf 'Param/Suite_1.\n  Case/0\n  12:34:56\n  two words\n  Case:bad\n'
      printf '  Next_2\n'
      exit 0 ;;
  esac
  printf 'Discovery.\n  First\n'
  case "${DISCOVERY_MODE:-single}" in
    stdout) printf '  12:34:56 runtime: diagnostic line\n' ;;
    stdout_plain) printf '12:34:56 runtime: diagnostic line\n' ;;
    stderr) printf 'StderrSuite.\n  StderrCase\n' >&2 ;;
  esac
  if [[ "${DISCOVERY_MODE:-single}" != single ]]; then
    printf '  Second\n'
  fi
  exit 0
fi
test_name=${1#--gtest_filter=}
case "$test_name" in
  Discovery.First|Discovery.Second|Param/Suite_1.Case/0|Param/Suite_1.Next_2) ;;
  *) printf '[  ERROR ] GC_UNIT_FILTER matched no test: %s\n' "$test_name" >&2; exit 1 ;;
esac
printf '[  RUN   ] %s\n[  PASS  ] %s\n' "$test_name" "$test_name"
printf '[========] 1 tests: 1 passed, 0 failed\n' >"${GC_UNIT_TALLY_FILE:?}"
EOF
chmod +x "$TMP/discovery"

discovery_failures=0
check_discovery() {
  local mode=$1 kind=$2 jobs=$3 expected_rc=$4 expected_invalid=$5 names=$6
  local out="$TMP/discovery-$mode-$kind-$jobs" rc=0 case_failed=0
  local main="$TMP/discovery" publication="$TMP/publication"
  if [[ "$kind" == publication ]]; then
    main="$TMP/main"
    publication="$TMP/discovery"
  fi
  DISCOVERY_MODE="$mode" GC_UNIT_JOBS="$jobs" GC_UNIT_TEST_TIMEOUT=10 \
    bash "$RUNNER" "$main" "$publication" "$out" "$TMP" >"$out.log" 2>&1 || rc=$?
  # Keep assertions nonfatal within a case: mutation evidence must show the
  # target invariant itself was evaluated, not only an earlier rc assertion.
  discovery_assert() {
    local label=$1
    shift
    if "$@"; then
      echo "DISCOVERY_ASSERT_PASS $mode/$kind/$jobs $label"
    else
      echo "DISCOVERY_ASSERT_FAIL $mode/$kind/$jobs $label"
      case_failed=1
    fi
  }
  echo "DISCOVERY_RUN $mode/$kind/$jobs rc=$rc"
  discovery_assert exit-status test "$rc" -eq "$expected_rc"
  printf '%s' "$names" >"$out.expected"
  discovery_assert names diff -u "$out.expected" "$out/test-lists/$kind.txt"
  local main_invalid=0 publication_invalid=0
  if [[ "$kind" == main ]]; then main_invalid=$expected_invalid; else publication_invalid=$expected_invalid; fi
  discovery_assert invalid-count /usr/bin/grep -qxF \
    "GC_UNIT_LIST_INVALID lines=$expected_invalid main=$main_invalid publication=$publication_invalid" "$out.log"
  if [[ "$expected_rc" -eq 0 ]]; then
    local total=$(( $(wc -l <"$out.expected") + 2 ))
    discovery_assert real-cases-completed /usr/bin/grep -qxF \
      "[========] $total tests: $total passed, 0 failed" "$out/parallel_tally.txt"
    discovery_assert complete /usr/bin/grep -qxF 'GC_UNIT_INCOMPLETE tests=0' "$out.log"
  elif [[ "$mode" == empty ]]; then
    discovery_assert empty-rejected /usr/bin/grep -qF 'GC_UNIT_LIST_TESTS_EMPTY ' "$out.log"
  fi
  if [[ "$mode" == stdout ]]; then
    discovery_assert diagnostic-preserved /usr/bin/grep -qxF \
      '  12:34:56 runtime: diagnostic line' "$out/test-lists/$kind.raw.invalid"
  elif [[ "$mode" == stdout_plain ]]; then
    discovery_assert diagnostic-preserved /usr/bin/grep -qxF \
      '12:34:56 runtime: diagnostic line' "$out/test-lists/$kind.raw.invalid"
  elif [[ "$mode" == stderr ]]; then
    discovery_assert stderr-preserved /usr/bin/grep -qxF \
      'StderrSuite.' "$out/test-lists/$kind.raw.stderr"
  fi
  echo "DISCOVERY_CASE $mode/$kind/$jobs failed=$case_failed"
  discovery_failures=$((discovery_failures + case_failed))
}

for kind in main publication; do
  for jobs in 1 2; do
    check_discovery single "$kind" "$jobs" 0 0 $'Discovery.First\n'
    check_discovery clean "$kind" "$jobs" 0 0 $'Discovery.First\nDiscovery.Second\n'
    check_discovery stdout "$kind" "$jobs" 0 1 $'Discovery.First\nDiscovery.Second\n'
    check_discovery stdout_plain "$kind" "$jobs" 0 1 $'Discovery.First\nDiscovery.Second\n'
    check_discovery stderr "$kind" "$jobs" 0 0 $'Discovery.First\nDiscovery.Second\n'
    check_discovery empty "$kind" "$jobs" 2 0 ''
    check_discovery grammar "$kind" "$jobs" 0 8 $'Param/Suite_1.Case/0\nParam/Suite_1.Next_2\n'
  done
done
# A failed list command must still expose its stderr diagnostic to the caller.
rc=0
DISCOVERY_MODE=failure bash "$RUNNER" "$TMP/discovery" "$TMP/publication" \
  "$TMP/list-failure" "$TMP" >"$TMP/list-failure.log" 2>&1 || rc=$?
[[ "$rc" -eq 2 ]]
/usr/bin/grep -qxF 'GC_UNIT_LIST_TESTS_FAIL main_rc=7 publication_rc=0' "$TMP/list-failure.log"
/usr/bin/grep -qxF '[main stderr] listing failed: diagnostic' "$TMP/list-failure.log"
echo "DISCOVERY_FAILURE_DIAGNOSTIC_PASS"
[[ "$discovery_failures" -eq 0 ]]
echo "PARALLEL_RUNNER_TEST_OK"
