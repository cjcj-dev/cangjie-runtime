#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
RUNNER="$ROOT/runtime/tests/gc_unit/run_parallel_tests.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

make_fake() {
  local path=$1 suite=$2 first=$3 second=$4
  sed \
    -e "s/@SUITE@/$suite/g" \
    -e "s/@FIRST@/$first/g" \
    -e "s/@SECOND@/$second/g" \
    "$ROOT/runtime/tests/gc_unit/parallel_runner_fixture.sh.in" >"$path"
  chmod +x "$path"
}

make_fake "$TMP/main" MainSuite PassOne FailWhenRequested
make_fake "$TMP/publication" PublicationSuite PassTwo PassThree

GC_UNIT_JOBS=2 GC_UNIT_TEST_TIMEOUT=10 GC_UNIT_FAKE_FAIL=MainSuite.FailWhenRequested \
  GC_UNIT_TALLY_FILE="$TMP/red.tally" \
  bash "$RUNNER" "$TMP/main" "$TMP/publication" "$TMP/red" "$TMP" >"$TMP/red.log" 2>&1 && exit 1
/usr/bin/grep -qxF '[========] 4 tests: 3 passed, 1 failed' "$TMP/red.tally"
/usr/bin/grep -qxF '[  FAILED  ] MainSuite.FailWhenRequested' "$TMP/red.log"
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

# A process terminated after its RUN token has no valid end token or tally.
# It must be reported as exactly one incomplete item and keep the total red.
GC_UNIT_JOBS=2 GC_UNIT_TEST_TIMEOUT=10 GC_UNIT_FAKE_KILL=MainSuite.PassOne \
  GC_UNIT_TALLY_FILE="$TMP/killed.tally" \
  bash "$RUNNER" "$TMP/main" "$TMP/publication" "$TMP/killed" "$TMP" \
  >"$TMP/killed.log" 2>&1 && exit 1
/usr/bin/grep -qxF '[========] 4 tests: 3 passed, 1 failed' "$TMP/killed.tally"
/usr/bin/grep -qxF '[  INCOMPLETE ] MainSuite.PassOne' "$TMP/killed.log"
/usr/bin/grep -qxF 'GC_UNIT_INCOMPLETE tests=1' "$TMP/killed.log"
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
echo "PARALLEL_RUNNER_TEST_OK"
