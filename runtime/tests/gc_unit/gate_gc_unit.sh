#!/usr/bin/env bash
# GC unit gate: the C++ suite fails closed; language-level tests normally run
# in the same invocation.  A source build may defer them until its matching std
# exists, but that is recorded separately and must later become LANGUAGE_DONE.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
SCRIPT="$SRC/run_standalone.sh"
FINALIZER_SCRIPT="$SRC/run_finalizer_trigger.sh"
PHASE_ENTRY_SCRIPT="$SRC/run_phase_entry_trigger.sh"
SEGMENTED_MANAGED_SCRIPT="$SRC/run_segmented_array_managed.sh"
STATUS_FILE="${GC_UNIT_GATE_STATUS:-}"
if [[ -z "$STATUS_FILE" && -n "${GCV2_RUNTIME_CONFIG:-}" ]]; then
  STATUS_FILE="$ROOT/runtime/output/temp/$GCV2_RUNTIME_CONFIG/gc_unit_gate.status"
elif [[ -z "$STATUS_FILE" && -n "${GCV2_RUNTIME_LIB_DIR:-}" ]]; then
  STATUS_FILE="$GCV2_RUNTIME_LIB_DIR/gc_unit_gate.status"
fi
STATUS_FILE="${STATUS_FILE:-$ROOT/runtime/output/gc_unit_gate.status}"
LANGUAGE_TEST_MODE="${GC_UNIT_GATE_LANGUAGE_TESTS:-all}"

GATE_STATE=FAIL
CPP_SUITE_STATE=NOT_RUN
CPP_SUITE_SOURCE=NOT_RUN
LANGUAGE_TESTS_STATE=NOT_RUN
FINALIZER_STATE=NOT_RUN
FINALIZER_SOURCE=NOT_RUN
PHASE_ENTRY_STATE=NOT_RUN
PHASE_ENTRY_SOURCE=NOT_RUN
SEGMENTED_MANAGED_STATE=NOT_RUN
SEGMENTED_MANAGED_SOURCE=NOT_RUN
SEGMENTED_MANAGED_CAN_RUN=0
OHOS_HOST_STATE=NOT_RUN
OHOS_HOST_SOURCE=NOT_RUN
OHOS_HOST_FILTERS=0
STATUS_REASON=UNEXPECTED_EXIT
TESTABLE_INTERNALS="${MRT_TESTABLE_INTERNALS:-0}"
OHOS_HOST="${MRT_GC_UNIT_OHOS_HOST:-0}"
RUNTIME_CONFIG_ID="${GCV2_RUNTIME_CONFIG:-explicit-lib-dir}"
RUNTIME_CONFIG_SIGNATURE=unrecorded
RUNTIME_SHA256=unrecorded
BOUNDSCHECK_SHA256=unrecorded

write_status() {
  local status_dir tmp
  status_dir="$(dirname "$STATUS_FILE")"
  mkdir -p "$status_dir"
  tmp="$STATUS_FILE.tmp.$$"
  {
    echo "SCHEMA_VERSION=1"
    echo "GATE=$GATE_STATE"
    echo "LANGUAGE_TEST_MODE=$LANGUAGE_TEST_MODE"
    echo "LANGUAGE_TESTS=$LANGUAGE_TESTS_STATE"
    echo "CPP_SUITE=$CPP_SUITE_STATE"
    echo "CPP_SUITE_SOURCE=$CPP_SUITE_SOURCE"
    echo "FINALIZER_TRIGGER=$FINALIZER_STATE"
    echo "FINALIZER_TRIGGER_SOURCE=$FINALIZER_SOURCE"
    echo "PHASE_ENTRY_TRIGGER=$PHASE_ENTRY_STATE"
    echo "PHASE_ENTRY_TRIGGER_SOURCE=$PHASE_ENTRY_SOURCE"
    echo "SEGMENTED_ARRAY_MANAGED=$SEGMENTED_MANAGED_STATE"
    echo "SEGMENTED_ARRAY_MANAGED_SOURCE=$SEGMENTED_MANAGED_SOURCE"
    echo "SEGMENTED_ARRAY_MANAGED_CAN_RUN=$SEGMENTED_MANAGED_CAN_RUN"
    echo "OHOS_HOST=$OHOS_HOST_STATE"
    echo "OHOS_HOST_SOURCE=$OHOS_HOST_SOURCE"
    echo "OHOS_HOST_FILTERS=$OHOS_HOST_FILTERS"
    echo "RUNTIME_CONFIG_ID=$RUNTIME_CONFIG_ID"
    echo "RUNTIME_CONFIG_SIGNATURE=$RUNTIME_CONFIG_SIGNATURE"
    echo "RUNTIME_SHA256=$RUNTIME_SHA256"
    echo "BOUNDSCHECK_SHA256=$BOUNDSCHECK_SHA256"
    echo "REASON=$STATUS_REASON"
  } >"$tmp"
  mv -f "$tmp" "$STATUS_FILE"
}

on_exit() {
  local rc=$?
  trap - EXIT
  write_status
  exit "$rc"
}
trap on_exit EXIT

case "$LANGUAGE_TEST_MODE" in
  all|defer|only) ;;
  *)
    STATUS_REASON=INVALID_LANGUAGE_TEST_MODE
    echo "GC_UNIT_GATE_FAIL: GC_UNIT_GATE_LANGUAGE_TESTS must be all, defer, or only (got '$LANGUAGE_TEST_MODE')" >&2
    exit 2
    ;;
esac

# This check exercises the gate's own contract.  It runs before any skip,
# runtime build, or capability probing so a broken contract cannot be hidden by
# NO_CJC or an explicit skip.  The contract fixture invokes a private copy of
# this gate; its recursion guard is intentionally scoped to that synthetic child
# only.
if [[ "${GC_UNIT_GATE_CONTRACT_SELFTEST:-0}" != "1" ]]; then
  # The fixture owns both product-shape inputs. The real post-build command
  # exports them, so merely assigning OHOS_HOST=0 still leaked TESTABLE=1 into
  # the fixture's first standalone pair and made that control arm fail early.
  if ! env -u MRT_TESTABLE_INTERNALS -u MRT_GC_UNIT_OHOS_HOST \
      bash "$SRC/test_gate_testable_contract.sh"; then
    echo "GC_UNIT_GATE_FAIL: testable gate contract failed" >&2
    exit 2
  fi
  echo "GATE_TESTABLE_CONTRACT_OK"
fi


# Invalidate any previous PASS before doing work.  The EXIT trap covers normal
# failures, but it cannot run after SIGKILL or machine loss; leaving an old PASS
# readable during a new run would let a composition gate accept stale evidence.
STATUS_REASON=STARTED
write_status

if [[ "${GC_UNIT_GATE_SKIP:-0}" == "1" ]]; then
  GATE_STATE=NOT_RUN
  STATUS_REASON=EXPLICIT_SKIP
  echo "GC_UNIT_GATE_NOT_RUN reason=EXPLICIT_SKIP status=$STATUS_FILE"
  exit 0
fi

case "$TESTABLE_INTERNALS" in
  0|1) ;;
  *)
    STATUS_REASON=INVALID_TESTABLE_INTERNALS
    echo "GC_UNIT_GATE_FAIL: MRT_TESTABLE_INTERNALS must be 0 or 1 (got '$TESTABLE_INTERNALS')" >&2
    exit 2
    ;;
esac

case "$OHOS_HOST" in
  0|1) ;;
  *)
    STATUS_REASON=INVALID_OHOS_HOST
    echo "GC_UNIT_GATE_FAIL: MRT_GC_UNIT_OHOS_HOST must be 0 or 1 (got '$OHOS_HOST')" >&2
    exit 2
    ;;
esac

if [[ ! -d "$SRC" ]]; then
  echo "GC_UNIT_GATE_FAIL: missing $SRC" >&2
  exit 2
fi
if [[ "$LANGUAGE_TEST_MODE" != "only" && ! -f "$SCRIPT" ]]; then
  echo "GC_UNIT_GATE_FAIL: missing run_standalone.sh" >&2
  exit 2
fi
if [[ ! -f "$FINALIZER_SCRIPT" || ! -f "$SRC/finalizer_trigger.cj" ||
      ! -f "$PHASE_ENTRY_SCRIPT" || ! -f "$SRC/phase_entry_trigger.cj" ||
      ! -f "$SRC/phase_entry_major.cj" ]]; then
  echo "GC_UNIT_GATE_FAIL: missing end-to-end language-level test" >&2
  exit 2
fi
if [[ "$TESTABLE_INTERNALS" == "1" &&
      ( ! -f "$SEGMENTED_MANAGED_SCRIPT" || ! -f "$SRC/segmented_array_managed.cj" ) ]]; then
  # TESTABLE_INTERNALS is an explicit product/test contract.  Do this check
  # before capability probing so a missing managed fixture cannot be hidden by
  # a product SO that was built without the test hook.
  echo "GC_UNIT_GATE_FAIL: missing managed segmented-array language-level test" >&2
  exit 2
fi
if [[ "$LANGUAGE_TEST_MODE" != "only" && ! -f "$SRC/test_defect_regressions.cpp" ]]; then
  echo "GC_UNIT_GATE_FAIL: missing defect regression suite (Phase 2)" >&2
  exit 2
fi

if [[ -n "${GCV2_RUNTIME_CONFIG:-}" ]]; then
  resolved_config_lib=$(bash "$ROOT/runtime/build/resolve_runtime_output.sh" \
    "$ROOT/runtime" "$GCV2_RUNTIME_CONFIG") || {
      STATUS_REASON=RUNTIME_CONFIG_MISMATCH
      exit 2
  }
  if [[ -n "${GCV2_RUNTIME_LIB_DIR:-}" &&
        "$(realpath "$GCV2_RUNTIME_LIB_DIR")" != "$resolved_config_lib" ]]; then
    STATUS_REASON=RUNTIME_CONFIG_MISMATCH
    echo "GC_UNIT_GATE_FAIL: explicit library directory does not match GCV2_RUNTIME_CONFIG" >&2
    exit 2
  fi
  GCV2_RUNTIME_LIB_DIR="$resolved_config_lib"
  GCV2_RUNTIME_OUTPUT_ROOT="$ROOT/runtime/output/temp/$GCV2_RUNTIME_CONFIG"
  export GCV2_RUNTIME_LIB_DIR
  export GCV2_RUNTIME_OUTPUT_ROOT
elif [[ -z "${GCV2_RUNTIME_LIB_DIR:-}" ]]; then
  STATUS_REASON=MISSING_RUNTIME_CONFIG
  echo "GC_UNIT_GATE_FAIL: set GCV2_RUNTIME_CONFIG or GCV2_RUNTIME_LIB_DIR" >&2
  exit 2
fi
if [[ -z "${GCV2_RUNTIME_LIB_DIR:-}" || ! -f "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" ]]; then
  STATUS_REASON=MISSING_RUNTIME
  echo "GC_UNIT_GATE_FAIL: no libcangjie-runtime.so (set GCV2_RUNTIME_LIB_DIR)" >&2
  exit 2
fi
if [[ -n "${GCV2_RUNTIME_CONFIG:-}" ]]; then
  RUNTIME_CONFIG_ID="$GCV2_RUNTIME_CONFIG"
  config_manifest="$ROOT/runtime/output/temp/$RUNTIME_CONFIG_ID/runtime-build-config.txt"
  RUNTIME_CONFIG_SIGNATURE=$(/usr/bin/sed -n 's/^CONFIG_SIGNATURE_SHA256=//p' "$config_manifest")
fi
if [[ -z "${GCV2_RUNTIME_OUTPUT_ROOT:-}" ]]; then
  GCV2_RUNTIME_OUTPUT_ROOT=$(realpath -m "$GCV2_RUNTIME_LIB_DIR/../..")
  export GCV2_RUNTIME_OUTPUT_ROOT
fi
RUNTIME_SHA256=$(sha256sum "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" | awk '{print $1}')
if [[ -f "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" ]]; then
  BOUNDSCHECK_SHA256=$(sha256sum "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" | awk '{print $1}')
fi
echo "GC_UNIT_RUNTIME_IDENTITY config=$RUNTIME_CONFIG_ID signature=$RUNTIME_CONFIG_SIGNATURE runtime_sha256=$RUNTIME_SHA256 boundscheck_sha256=$BOUNDSCHECK_SHA256 lib_dir=$GCV2_RUNTIME_LIB_DIR"

# The OHOS-host arm is a separate product shape and a focused three-process
# suite. It must neither borrow the default suite's stamp nor depend on a Cangjie
# SDK. run_standalone verifies the product receipt, product/test symbol
# ownership, dispatch disassembly, and one exact completion token per filter.
if [[ "$OHOS_HOST" == "1" ]]; then
  export GC_UNIT_OUT="${GC_UNIT_OUT:-$SRC/build_ohos_host}"
  mkdir -p "$GC_UNIT_OUT"
  OHOS_RECEIPT="${GC_UNIT_OHOS_HOST_RECEIPT:-$GC_UNIT_OUT/ohos_host.receipt}"
  rm -f "$OHOS_RECEIPT"
  OHOS_HOST_STATE=FAIL
  OHOS_HOST_SOURCE=FRESH
  STATUS_REASON=OHOS_HOST_FAILURE
  GC_UNIT_TIMEOUT="${GC_UNIT_TIMEOUT:-600}"
  set +e
  timeout "$GC_UNIT_TIMEOUT" bash "$SCRIPT" >"$GC_UNIT_OUT/ohos_host_gate.log" 2>&1
  ohos_rc=$?
  set -e
  if [[ $ohos_rc -eq 124 ]]; then
    STATUS_REASON=OHOS_HOST_TIMEOUT
    echo "GC_UNIT_GATE_FAIL: OHOS-host suite timed out after ${GC_UNIT_TIMEOUT}s" >&2
    exit 6
  fi
  if [[ $ohos_rc -ne 0 || ! -f "$OHOS_RECEIPT" ]] ||
      ! /usr/bin/grep -qx 'RESULT=PASS' "$OHOS_RECEIPT" ||
      ! /usr/bin/grep -qx 'FILTER_MAJOR=PASS' "$OHOS_RECEIPT" ||
      ! /usr/bin/grep -qx 'FILTER_POST=PASS' "$OHOS_RECEIPT" ||
      ! /usr/bin/grep -qx 'FILTER_EMPTY=PASS' "$OHOS_RECEIPT"; then
    echo "GC_UNIT_GATE_FAIL: OHOS-host receipt incomplete (rc=$ohos_rc receipt=$OHOS_RECEIPT)" >&2
    tail -20 "$GC_UNIT_OUT/ohos_host_gate.log" >&2 || true
    exit 1
  fi
  OHOS_HOST_STATE=PASS
  OHOS_HOST_FILTERS=3
  CPP_SUITE_STATE=PASS
  CPP_SUITE_SOURCE=FRESH
  GATE_STATE=PASS
  STATUS_REASON=PASS
  echo "GC_UNIT_GATE_OHOS_HOST_OK filters=3 receipt=$OHOS_RECEIPT status=$STATUS_FILE"
  exit 0
fi

# Resolve cjc before the stamp fast path.  Otherwise a stamp from an earlier
# compiler-equipped build could turn today's missing compiler into a cached
# PASS instead of the required visible NOT_RUN state.
CJC_BIN="${CJC:-${CANGJIE_HOME:-}/bin/cjc}"
FINALIZER_CAN_RUN=0
if [[ "$LANGUAGE_TEST_MODE" == "only" ]]; then
  if [[ -z "${CANGJIE_HOME:-}" || ! -x "$CANGJIE_HOME/bin/cjc" ]]; then
    STATUS_REASON=LANGUAGE_SDK_MISSING
    echo "GC_UNIT_GATE_FAIL: only mode requires CANGJIE_HOME with an executable bin/cjc" >&2
    exit 2
  fi
  if [[ -n "${CJC:-}" && "$(readlink -f "$CJC")" != "$(readlink -f "$CANGJIE_HOME/bin/cjc")" ]]; then
    STATUS_REASON=LANGUAGE_SDK_MISMATCH
    echo "GC_UNIT_GATE_FAIL: only mode requires CJC to be CANGJIE_HOME/bin/cjc" >&2
    exit 2
  fi
  CJC_BIN="$CANGJIE_HOME/bin/cjc"
  FINALIZER_CAN_RUN=1
  export CJC="$CJC_BIN"
elif [[ -x "$CJC_BIN" ]]; then
  FINALIZER_CAN_RUN=1
  export CJC="$CJC_BIN"
fi

export GC_UNIT_OUT="${GC_UNIT_OUT:-$SRC/build_standalone}"

STAMP="$GC_UNIT_OUT/.gate_stamp"
SO="$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so"
# ELF versioned exports print as `name@@VERSION`; accept both that and the
# unversioned form. An exact `$` anchor after the bare name misses the
# versioned export and silently keeps SEGMENTED_MANAGED_CAN_RUN at 0.
if nm -D "$SO" | /usr/bin/grep -E '[[:space:]]CJ_MRT_SetLargeArrayInitTestHooks(@@[^[:space:]]+)?$' >/dev/null; then
  SEGMENTED_MANAGED_CAN_RUN=1
  if [[ ! -f "$SEGMENTED_MANAGED_SCRIPT" || ! -f "$SRC/segmented_array_managed.cj" ]]; then
    echo "GC_UNIT_GATE_FAIL: product SO exposes segmented-array test hooks but the managed test is missing" >&2
    exit 2
  fi
elif [[ "$TESTABLE_INTERNALS" == "1" ]]; then
  # In a TESTABLE build the hook is part of the same contract as the managed
  # fixture.  Treat its absence as a configuration error instead of silently
  # converting the managed phase to NOT_RUN.
  echo "GC_UNIT_GATE_FAIL: TESTABLE_INTERNALS=1 but product SO lacks segmented-array test hooks" >&2
  exit 2
fi

run_language_tests() {
  LANGUAGE_TESTS_STATE=LANGUAGE_RUNNING

  # Root classification has a language-visible consequence that a C++ fixture
  # alone cannot prove: unreachable objects must actually execute ~init.
  FINALIZER_STATE=FAIL
  FINALIZER_SOURCE=FRESH
  STATUS_REASON=FINALIZER_TRIGGER_FAILURE
  if ! bash "$FINALIZER_SCRIPT"; then
    echo "GC_UNIT_GATE_FAIL: end-to-end finalizer trigger test failed" >&2
    return 1
  fi
  FINALIZER_STATE=PASS

  PHASE_ENTRY_STATE=FAIL
  PHASE_ENTRY_SOURCE=FRESH
  STATUS_REASON=PHASE_ENTRY_TRIGGER_FAILURE
  if ! bash "$PHASE_ENTRY_SCRIPT"; then
    echo "GC_UNIT_GATE_FAIL: forwarding-carrier phase entry test failed" >&2
    return 1
  fi
  PHASE_ENTRY_STATE=PASS

  if [[ $SEGMENTED_MANAGED_CAN_RUN -eq 1 ]]; then
    SEGMENTED_MANAGED_STATE=FAIL
    SEGMENTED_MANAGED_SOURCE=FRESH
    STATUS_REASON=SEGMENTED_ARRAY_MANAGED_FAILURE
    if ! bash "$SEGMENTED_MANAGED_SCRIPT"; then
      echo "GC_UNIT_GATE_FAIL: managed segmented-array product entry test failed" >&2
      return 1
    fi
    SEGMENTED_MANAGED_STATE=PASS
  fi

  LANGUAGE_TESTS_STATE=LANGUAGE_DONE
}

# only is deliberately fresh and bypasses the all-mode stamp: a stamp made by
# another compiler/std pair cannot prove that this source SDK ran the tests.
if [[ "$LANGUAGE_TEST_MODE" == "only" ]]; then
  if ! run_language_tests; then
    exit 1
  fi
  GATE_STATE=PASS
  STATUS_REASON=PASS
  echo "GC_UNIT_GATE_LANGUAGE_OK mode=only status=$STATUS_FILE"
  exit 0
fi

# Two source lists name this suite: CMakeLists.txt (behind MRT_GC_UNIT_TESTS, default OFF) and
# run_standalone.sh (the one that actually runs).  test_z_forwarding_life.cpp sat in the first and
# not the second, so its three tests had never executed once.  A file present but not compiled looks
# exactly like a file that passes.
in_cmake=$(grep -oE 'test_[a-z0-9_]+\.cpp' "$SRC/CMakeLists.txt" | sort -u)
in_script=$(grep -oE 'test_[a-z0-9_]+\.cpp' "$SCRIPT" | sort -u)
drift=$(comm -3 <(echo "$in_cmake") <(echo "$in_script"))
if [[ -n "$drift" ]]; then
  echo "GC_UNIT_GATE_FAIL: CMakeLists.txt and run_standalone.sh disagree on the test list:" >&2
  echo "$drift" | sed 's/^/  /' >&2
  exit 5
fi

# The suite links -lcangjie-runtime, so it measures the .so, not the source tree -- but it compiles
# the product *headers* into its own binary.  A .so older than the sources therefore gives every test
# a new inline half and an old compiled half, which is not a control arm, it is a third program.
# (Observed 2026-08-18: exactly this skew flipped the Remset results and read as a fresh regression.)
newer_src=$(find "$ROOT/runtime/src" \( -name '*.cpp' -o -name '*.h' \) -newer "$SO" -print -quit 2>/dev/null)
if [[ -n "$newer_src" ]]; then
  echo "GC_UNIT_GATE_FAIL: $SO is older than runtime sources -- rebuild before trusting this suite:" >&2
  echo "$newer_src" | sed 's/^/  /' >&2
  exit 4
fi

# Skip when nothing under test has changed since the last green run.  Without this the gate rebuilds
# ~20 translation units on every incremental runtime build and gets switched off within a day, which
# is how this suite spent its whole life behind a default-OFF flag.
if [[ -f "$STAMP" && "$STAMP" -nt "$SO" ]]; then
  # Every file in the suite directory, not just sources: known_failures.txt, this script and the two
  # test lists all change the verdict.  Keying the cache on *.cpp/*.h alone meant editing a waiver
  # was never re-checked -- caught by firing that arm on purpose and watching it read green.
  newer=$(find "$SRC" -path "$GC_UNIT_OUT" -prune -o -type f -newer "$STAMP" -print -quit 2>/dev/null)
  if [[ -z "$newer" ]]; then
    CPP_SUITE_STATE=PASS
    CPP_SUITE_SOURCE=CACHE
    if [[ "$LANGUAGE_TEST_MODE" == "defer" ]]; then
      LANGUAGE_TESTS_STATE=LANGUAGE_DEFERRED
      GATE_STATE=PASS
      STATUS_REASON=LANGUAGE_DEFERRED
      echo "GC_UNIT_GATE_OK language=deferred cpp_suite=PASS(cache) status=$STATUS_FILE"
      exit 0
    fi
    if [[ $FINALIZER_CAN_RUN -eq 0 ]]; then
      GATE_STATE=NOT_RUN
      STATUS_REASON=NO_CJC
      echo "GC_UNIT_GATE_NOT_RUN reason=NO_CJC cpp_suite=PASS(cache) status=$STATUS_FILE"
      exit 0
    fi
    FINALIZER_STATE=PASS
    FINALIZER_SOURCE=CACHE
    PHASE_ENTRY_STATE=PASS
    PHASE_ENTRY_SOURCE=CACHE
    if [[ $SEGMENTED_MANAGED_CAN_RUN -eq 1 ]]; then
      SEGMENTED_MANAGED_STATE=PASS
      SEGMENTED_MANAGED_SOURCE=CACHE
    fi
    LANGUAGE_TESTS_STATE=LANGUAGE_DONE
    GATE_STATE=PASS
    STATUS_REASON=CACHED_PASS
    echo "GC_UNIT_GATE_OK (cached: runtime and suite both older than last green run) status=$STATUS_FILE"
    exit 0
  fi
fi

KNOWN="$SRC/known_failures.txt"
OUT="$GC_UNIT_OUT/gate_run.log"
TALLY="$GC_UNIT_OUT/gate_tally.txt"
mkdir -p "$GC_UNIT_OUT"
# The tally is completion evidence, not a cache.  Remove it before every run so an early compiler
# error, signal or timeout cannot make this invocation consume a previous run's success.
rm -f "$TALLY"
export GC_UNIT_TALLY_FILE="$TALLY"
# Timeout, because a hanging test is not a failing test. The suite runs in tens of seconds; the one
# time it did not, a perturbation reintroduced an unbounded probe loop and the *build* hung rather
# than the gate reporting anything -- POST_BUILD inherits the hang, so `cmake --build` never
# returned and the only signal was a 600s wall. 600s is far above the observed wall, so it cannot
# fire on a slow machine.
GC_UNIT_TIMEOUT="${GC_UNIT_TIMEOUT:-600}"
set +e
timeout "$GC_UNIT_TIMEOUT" bash "$SCRIPT" >"$OUT" 2>&1
suite_rc=$?
set -e
if [[ $suite_rc -eq 124 ]]; then
  CPP_SUITE_STATE=FAIL
  CPP_SUITE_SOURCE=FRESH
  STATUS_REASON=CPP_SUITE_TIMEOUT
  echo "GC_UNIT_GATE_FAIL: suite did not finish within ${GC_UNIT_TIMEOUT}s -- a test is hung, not slow" >&2
  echo "  last lines of $OUT:" >&2
  tail -5 "$OUT" >&2
  exit 6
fi
tail -20 "$OUT"

# The suite's own exit code is not the gate's verdict: it is nonzero whenever anything fails,
# including the failures that are written down.  The verdict is the set difference, in both
# directions.
# `|| true` on both: with zero failures grep exits 1, and under `set -e` + `pipefail` that kills the
# gate *after* the suite has already printed a clean tally -- a guard that only survives when it has
# something to complain about.  Observed here on the first all-green run.
actual=$(grep -oE '^\[  FAIL  \] [A-Za-z0-9_]+\.[A-Za-z0-9_]+' "$OUT" | sed -E 's/^.*\] //' | sort -u || true)
allowed=$(grep -vE '^\s*(#|$)' "$KNOWN" | tr -d ' \t' | sort -u || true)

# Parse sanity, not an empty-list check: an empty allowlist is the normal, desired state.  Only a
# file that HAS non-comment lines yet parses to nothing indicates the reader is broken.
if [[ -z "$allowed" ]] && grep -qE '^[A-Za-z]' "$KNOWN"; then
  echo "GC_UNIT_GATE_FAIL: known_failures.txt has entries but parsed to nothing -- reader is broken" >&2
  exit 3
fi
# A grep that matches nothing and a run that never happened produce the same empty string, and this
# campaign has already read one for the other.  Require one exact tally from the suite's independent
# evidence file before trusting an empty set.  Parsing the merged stdout/stderr log here reintroduces
# the atexit interleaving failure this file exists to prevent.
if [[ ! -f "$TALLY" ]] || ! grep -qxE '\[========\] [0-9]+ tests: [0-9]+ passed, [0-9]+ failed' "$TALLY" ||
    [[ $(wc -l <"$TALLY") -ne 1 ]]; then
  CPP_SUITE_STATE=FAIL
  CPP_SUITE_SOURCE=FRESH
  STATUS_REASON=CPP_SUITE_INCOMPLETE
  echo "GC_UNIT_GATE_FAIL: no valid independent tally -- the suite did not run to completion (rc=$suite_rc)" >&2
  exit 3
fi
tally_tests=$(sed -E 's/^\[========\] ([0-9]+) tests: ([0-9]+) passed, ([0-9]+) failed$/\1/' "$TALLY")
tally_passed=$(sed -E 's/^\[========\] ([0-9]+) tests: ([0-9]+) passed, ([0-9]+) failed$/\2/' "$TALLY")
tally_failed=$(sed -E 's/^\[========\] ([0-9]+) tests: ([0-9]+) passed, ([0-9]+) failed$/\3/' "$TALLY")
actual_count=0
if [[ -n "$actual" ]]; then
  actual_count=$(echo "$actual" | wc -l)
fi
if [[ $((tally_passed + tally_failed)) -ne $tally_tests || $actual_count -ne $tally_failed ]]; then
  CPP_SUITE_STATE=FAIL
  CPP_SUITE_SOURCE=FRESH
  STATUS_REASON=CPP_SUITE_EVIDENCE_MISMATCH
  echo "GC_UNIT_GATE_FAIL: tally/failure evidence mismatch (tests=$tally_tests passed=$tally_passed failed=$tally_failed parsed_failures=$actual_count)" >&2
  exit 3
fi

new_fail=$(comm -23 <(echo "$actual") <(echo "$allowed") || true)
fixed=$(comm -13 <(echo "$actual") <(echo "$allowed") || true)

rc=0
if [[ -n "$new_fail" ]]; then
  echo "GC_UNIT_GATE_FAIL: failures not in known_failures.txt:" >&2
  echo "$new_fail" | sed 's/^/  /' >&2
  rc=1
fi
if [[ -n "$fixed" ]]; then
  echo "GC_UNIT_GATE_FAIL: these are listed as failing but now pass -- delete their entries:" >&2
  echo "$fixed" | sed 's/^/  /' >&2
  rc=1
fi
if [[ $rc -ne 0 ]]; then
  CPP_SUITE_STATE=FAIL
  CPP_SUITE_SOURCE=FRESH
  STATUS_REASON=CPP_SUITE_FAILURE
  echo "  full log: $OUT" >&2
  exit "$rc"
fi

CPP_SUITE_STATE=PASS
CPP_SUITE_SOURCE=FRESH

if [[ "$LANGUAGE_TEST_MODE" == "defer" ]]; then
  LANGUAGE_TESTS_STATE=LANGUAGE_DEFERRED
  GATE_STATE=PASS
  STATUS_REASON=LANGUAGE_DEFERRED
  echo "GC_UNIT_GATE_OK language=deferred cpp_suite=PASS(fresh) status=$STATUS_FILE"
  exit 0
fi

if [[ $FINALIZER_CAN_RUN -eq 0 ]]; then
  GATE_STATE=NOT_RUN
  STATUS_REASON=NO_CJC
  echo "GC_UNIT_GATE_NOT_RUN reason=NO_CJC cpp_suite=PASS(fresh) status=$STATUS_FILE"
  exit 0
fi

if ! run_language_tests; then
  exit 1
fi

GATE_STATE=PASS
STATUS_REASON=PASS
touch "$STAMP"
n_allowed=$(echo "$allowed" | grep -c . || true)
echo "GC_UNIT_GATE_OK tests=$tally_tests known_failures=$n_allowed (suite rc=$suite_rc) status=$STATUS_FILE"
