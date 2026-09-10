#!/usr/bin/env bash
# Run every registered GC unit test in its own process. The two executables
# share one global worker budget and are interleaved in the work manifest.
set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $0 MAIN_ELF PUBLICATION_ELF OUT_DIR RUNTIME_LIB_DIR" >&2
  exit 2
fi

MAIN_ELF=$1
PUBLICATION_ELF=$2
OUT=$3
RUNTIME_LIB_DIR=$4
JOBS="${GC_UNIT_JOBS:-$(nproc)}"
TEST_TIMEOUT="${GC_UNIT_TEST_TIMEOUT:-600}"
FINAL_TALLY="${GC_UNIT_TALLY_FILE:-}"

if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: GC_UNIT_JOBS must be a positive integer, got: $JOBS" >&2
  exit 2
fi
if [[ ! "$TEST_TIMEOUT" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: GC_UNIT_TEST_TIMEOUT must be a positive integer, got: $TEST_TIMEOUT" >&2
  exit 2
fi

LOG_DIR="$OUT/test-logs"
RC_DIR="$OUT/test-rc"
TALLY_DIR="$OUT/test-tallies"
LIST_DIR="$OUT/test-lists"
mkdir -p "$LOG_DIR" "$RC_DIR" "$TALLY_DIR" "$LIST_DIR"
find "$LOG_DIR" "$RC_DIR" "$TALLY_DIR" "$LIST_DIR" -type f -delete

list_tests() {
  local kind=$1 elf=$2 output=$3
  local -a extra_env=()
  if [[ "$kind" == main && -n "${GC_UNIT_MAIN_ENV:-}" ]]; then
    mapfile -t extra_env <<<"$GC_UNIT_MAIN_ENV"
  fi
  env "${extra_env[@]}" \
    LD_LIBRARY_PATH="$RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$elf" --gtest_list_tests >"$output" 2>&1
}

list_tests main "$MAIN_ELF" "$LIST_DIR/main.raw" &
main_list_pid=$!
list_tests publication "$PUBLICATION_ELF" "$LIST_DIR/publication.raw" &
publication_list_pid=$!
set +e
wait "$main_list_pid"
main_list_rc=$?
wait "$publication_list_pid"
publication_list_rc=$?
set -e
if [[ $main_list_rc -ne 0 || $publication_list_rc -ne 0 ]]; then
  echo "GC_UNIT_LIST_TESTS_FAIL main_rc=$main_list_rc publication_rc=$publication_list_rc" >&2
  sed 's/^/[main] /' "$LIST_DIR/main.raw" >&2
  sed 's/^/[publication] /' "$LIST_DIR/publication.raw" >&2
  exit 2
fi

parse_list() {
  awk '
    /^[^[:space:]#].*\.$/ {
      suite = $0
      sub(/[[:space:]]*#.*/, "", suite)
      sub(/\.$/, "", suite)
      next
    }
    /^[[:space:]]+/ && suite != "" {
      name = $0
      sub(/^[[:space:]]+/, "", name)
      sub(/[[:space:]]*#.*/, "", name)
      if (name != "") print suite "." name
    }
  ' "$1"
}

parse_list "$LIST_DIR/main.raw" >"$LIST_DIR/main.txt"
parse_list "$LIST_DIR/publication.raw" >"$LIST_DIR/publication.txt"
if [[ ! -s "$LIST_DIR/main.txt" || ! -s "$LIST_DIR/publication.txt" ]]; then
  echo "GC_UNIT_LIST_TESTS_EMPTY main=$(wc -l <"$LIST_DIR/main.txt") publication=$(wc -l <"$LIST_DIR/publication.txt")" >&2
  exit 2
fi

MANIFEST="$OUT/test-manifest.tsv"
: >"$MANIFEST"
exec 3<"$LIST_DIR/main.txt"
exec 4<"$LIST_DIR/publication.txt"
index=0
while true; do
  if read -r -u 3 main_test; then
    main_read_rc=0
  else
    main_read_rc=1
  fi
  if read -r -u 4 publication_test; then
    publication_read_rc=0
  else
    publication_read_rc=1
  fi
  if [[ $main_read_rc -ne 0 && $publication_read_rc -ne 0 ]]; then
    break
  fi
  if [[ $main_read_rc -eq 0 ]]; then
    printf 'main\t%s\t%06d\n' "$main_test" "$index" >>"$MANIFEST"
    index=$((index + 1))
  fi
  if [[ $publication_read_rc -eq 0 ]]; then
    printf 'publication\t%s\t%06d\n' "$publication_test" "$index" >>"$MANIFEST"
    index=$((index + 1))
  fi
done
exec 3<&-
exec 4<&-

run_one_test() {
  local record=$1 kind test index elf log rc_file tally_file rc
  IFS=$'\t' read -r kind test index <<<"$record"
  case "$kind" in
    main) elf=$MAIN_ELF ;;
    publication) elf=$PUBLICATION_ELF ;;
    *) return 2 ;;
  esac
  log="$LOG_DIR/${index}-${kind}.log"
  rc_file="$RC_DIR/${index}-${kind}.rc"
  tally_file="$TALLY_DIR/${index}-${kind}.txt"
  local -a extra_env=()
  if [[ "$kind" == main && -n "${GC_UNIT_MAIN_ENV:-}" ]]; then
    mapfile -t extra_env <<<"$GC_UNIT_MAIN_ENV"
  fi
  # List mode is an implementation detail of discovery. Never let an ambient
  # value turn an isolated test invocation back into another listing pass.
  unset GC_UNIT_LIST_TESTS
  set +e
  timeout "$TEST_TIMEOUT" env "${extra_env[@]}" \
    GC_UNIT_TALLY_FILE="$tally_file" \
    LD_LIBRARY_PATH="$RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$elf" "--gtest_filter=$test" >"$log" 2>&1
  rc=$?
  set -e
  printf '%d\n' "$rc" >"$rc_file"
  return 0
}
export -f run_one_test
export MAIN_ELF PUBLICATION_ELF RUNTIME_LIB_DIR LOG_DIR RC_DIR TALLY_DIR TEST_TIMEOUT
export GC_UNIT_MAIN_ENV="${GC_UNIT_MAIN_ENV:-}"

START=$(date +%s%N)
set +e
xargs -d '\n' -n 1 -P "$JOBS" bash -c 'run_one_test "$1"' _ <"$MANIFEST"
xargs_rc=$?
set -e
if [[ $xargs_rc -ne 0 ]]; then
  echo "GC_UNIT_XARGS_FAIL rc=$xargs_rc" >&2
  exit 2
fi

tests=0
passed=0
failed=0
incomplete=0
main_rc=0
publication_rc=0
failed_tests=()
incomplete_tests=()
suites_file="$OUT/test-suites.txt"
: >"$suites_file"
while IFS=$'\t' read -r kind test index; do
  tests=$((tests + 1))
  printf '%s\n' "${test%%.*}" >>"$suites_file"
  log="$LOG_DIR/${index}-${kind}.log"
  rc_file="$RC_DIR/${index}-${kind}.rc"
  tally_file="$TALLY_DIR/${index}-${kind}.txt"
  cat "$log"
  if [[ ! -f "$rc_file" ]]; then
    rc=125
  else
    rc=$(sed -n '1p' "$rc_file")
    if [[ ! "$rc" =~ ^[0-9]+$ ]] || [[ $(wc -l <"$rc_file") -ne 1 ]]; then
      rc=125
    fi
  fi
  # A process status alone is not completion evidence: list mode, an early
  # return, timeout, or signal can all omit the selected test's end state.
  # Accept only a matching log token plus the independently written one-test
  # tally. Everything else is an explicit incomplete failure.
  completed_pass=0
  completed_fail=0
  if [[ -f "$tally_file" ]] && [[ $(wc -l <"$tally_file") -eq 1 ]]; then
    if [[ "$rc" -eq 0 ]] &&
        /usr/bin/grep -F -q "[  PASS  ] $test" "$log" &&
        /usr/bin/grep -qxF '[========] 1 tests: 1 passed, 0 failed' "$tally_file"; then
      completed_pass=1
    elif [[ "$rc" -ne 0 ]] &&
        /usr/bin/grep -F -q "[  FAIL  ] $test" "$log" &&
        /usr/bin/grep -qxF '[========] 1 tests: 0 passed, 1 failed' "$tally_file"; then
      completed_fail=1
    fi
  fi
  if [[ "$completed_pass" -eq 1 ]]; then
    passed=$((passed + 1))
  else
    failed=$((failed + 1))
    failed_tests+=("$test")
    if [[ "$completed_fail" -ne 1 ]]; then
      incomplete=$((incomplete + 1))
      incomplete_tests+=("$test")
      # Persist the synthesized end state in the per-case log as well as the
      # aggregate stream, so every manifest row remains independently auditable.
      printf '[  FAIL  ] %s\n  isolated process incomplete rc=%d\n' "$test" "$rc" |
        tee -a "$log"
    fi
    if [[ "$kind" == main ]]; then
      main_rc=1
    else
      publication_rc=1
    fi
  fi
done <"$MANIFEST"

suite_count=$(sort -u "$suites_file" | wc -l)
END=$(date +%s%N)
elapsed_ms=$(( (END - START) / 1000000 ))
printf '[==========] %d tests from %d test suites ran.\n' "$tests" "$suite_count"
printf '[  PASSED  ] %d tests.\n' "$passed"
if [[ $failed -ne 0 ]]; then
  printf '[  FAILED  ] %d tests, listed below:\n' "$failed"
  for test in "${failed_tests[@]}"; do
    printf '[  FAILED  ] %s\n' "$test"
  done
fi
if [[ $incomplete -ne 0 ]]; then
  printf '[  INCOMPLETE ] %d tests, listed below:\n' "$incomplete"
  for test in "${incomplete_tests[@]}"; do
    printf '[  INCOMPLETE ] %s\n' "$test"
  done
fi
printf '[========] %d tests: %d passed, %d failed\n' "$tests" "$passed" "$failed" | tee "$OUT/parallel_tally.txt"
if [[ -n "$FINAL_TALLY" ]]; then
  if [[ "$FINAL_TALLY" != "$OUT/parallel_tally.txt" ]]; then
    cp "$OUT/parallel_tally.txt" "$FINAL_TALLY"
  fi
fi
printf 'GC_UNIT_PARALLEL jobs=%d tests=%d wall=%d.%03d\n' \
  "$JOBS" "$tests" "$((elapsed_ms / 1000))" "$((elapsed_ms % 1000))"
printf 'GC_UNIT_INCOMPLETE tests=%d\n' "$incomplete"
rc=0
if [[ $failed -ne 0 ]]; then
  rc=1
fi
printf 'GC_UNIT_RUN_DONE rc=%d main_rc=%d publication_rc=%d wall_ms=%d\n' \
  "$rc" "$main_rc" "$publication_rc" "$elapsed_ms"
exit "$rc"
