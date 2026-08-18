#!/usr/bin/env bash
# Fail-closed GC unit gate: suite must exist, build, and pass.
# Intended for kkk2 / CI wrappers. Exit non-zero if anything is missing.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
SCRIPT="$SRC/run_standalone.sh"

if [[ ! -d "$SRC" ]]; then
  echo "GC_UNIT_GATE_FAIL: missing $SRC" >&2
  exit 2
fi
if [[ ! -f "$SCRIPT" ]]; then
  echo "GC_UNIT_GATE_FAIL: missing run_standalone.sh" >&2
  exit 2
fi
if [[ ! -f "$SRC/test_defect_regressions.cpp" ]]; then
  echo "GC_UNIT_GATE_FAIL: missing defect regression suite (Phase 2)" >&2
  exit 2
fi

if [[ -z "${GCV2_RUNTIME_LIB_DIR:-}" ]]; then
  for cand in \
    "$ROOT/runtime/output/temp/lib/x86_64_Release" \
    "$ROOT/runtime/output/temp/lib/x86_64_Relwithdebinfo"; do
    if [[ -f "$cand/libcangjie-runtime.so" ]]; then
      export GCV2_RUNTIME_LIB_DIR="$cand"
      break
    fi
  done
fi
if [[ -z "${GCV2_RUNTIME_LIB_DIR:-}" || ! -f "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" ]]; then
  echo "GC_UNIT_GATE_FAIL: no libcangjie-runtime.so (set GCV2_RUNTIME_LIB_DIR)" >&2
  exit 2
fi

export GC_UNIT_OUT="${GC_UNIT_OUT:-$SRC/build_standalone}"

STAMP="$GC_UNIT_OUT/.gate_stamp"
SO="$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so"
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
    echo "GC_UNIT_GATE_OK (cached: runtime and suite both older than last green run)"
    exit 0
  fi
fi

KNOWN="$SRC/known_failures.txt"
OUT="$GC_UNIT_OUT/gate_run.log"
mkdir -p "$GC_UNIT_OUT"
set +e
bash "$SCRIPT" >"$OUT" 2>&1
suite_rc=$?
set -e
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
# campaign has already read one for the other.  Require the tally line before trusting an empty set.
if ! grep -qE '^\[========\] [0-9]+ tests:' "$OUT"; then
  echo "GC_UNIT_GATE_FAIL: no tally line in output -- the suite did not run to completion (rc=$suite_rc)" >&2
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
  echo "  full log: $OUT" >&2
  exit "$rc"
fi

touch "$STAMP"
n_allowed=$(echo "$allowed" | grep -c . || true)
n_tests=$(grep -oE '^\[========\] [0-9]+ tests' "$OUT" | grep -oE '[0-9]+' | head -1 || true)
echo "GC_UNIT_GATE_OK tests=$n_tests known_failures=$n_allowed (suite rc=$suite_rc)"
