#!/usr/bin/env bash
# Observe the real unit main: live runtime pool -> StopGCWork -> completion.
set -euo pipefail
src="$(cd "$(dirname "$0")" && pwd)"
elf="$(realpath "$1")"
lib="$(realpath "$2")"
out="$3"
mkdir -p "$out"
sha256sum "$elf" "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" > "$out/teardown-artifacts.sha256"
set +e
(
env -u GC_UNIT_LIST_TESTS -u GC_UNIT_ABORT_BEFORE -u GC_UNIT_TALLY_FILE   GC_UNIT_OTHER_VM_CHILD=RuntimeWorkers.ActivePoolBeforeHarnessShutdown   LD_LIBRARY_PATH="$lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"   timeout 120 gdb -nx -batch -x "$src/check_other_vm_teardown.py"   --args "$elf" --gtest_filter=RuntimeWorkers.ActivePoolBeforeHarnessShutdown   > "$out/teardown.log" 2>&1
echo "$?" > "$out/teardown.rc"
) &
for phase in exited live; do
  (
    args=()
    [[ "$phase" = live ]] && args+=(--live)
    env -u GC_UNIT_LIST_TESTS -u GC_UNIT_ABORT_BEFORE -u GC_UNIT_TALLY_FILE \
      timeout 120 python3 "$src/check_teardown_exit.py" "$elf" "$lib" "${args[@]}" \
      > "$out/teardown-$phase.log" 2>&1
    echo "$?" > "$out/teardown-$phase.rc"
  ) &
done
wait
set -e
rc=$(cat "$out/teardown.rc")
if [[ "$rc" = 0 ]] && ! grep -Fq 'GC_UNIT_OTHER_VM_OKIDOKI RuntimeWorkers.ActivePoolBeforeHarnessShutdown' "$out/teardown.log"; then
  echo 'ASSERT_TEARDOWN_SENTINEL FAIL' >> "$out/teardown.log"
  rc=1
fi
for phase in exited live; do
  phase_rc=$(cat "$out/teardown-$phase.rc")
  echo "TEARDOWN_CONSTRUCT_${phase^^}_RC=$phase_rc"
  if [[ "$phase_rc" = 77 ]]; then
    echo "TEARDOWN_CONSTRUCTION_NOT_RUN phase=$phase ptrace unavailable; not counted as PASS"
    [[ "$rc" = 0 ]] && rc=77
    continue
  fi
  expected_rc=0; expected_assertion=PASS
  [[ "$phase" = live ]] && { expected_rc=1; expected_assertion=FAIL; }
  if [[ "$phase_rc" != "$expected_rc" ]] ||
      ! grep -Fxq "ASSERT_TEARDOWN_BEFORE_SENTINEL samples=1 $expected_assertion" "$out/teardown-$phase.log" ||
      ! grep -Fxq 'TEARDOWN_CONSTRUCT_EXECUTED product_rc=0' "$out/teardown-$phase.log"; then
    [[ "$rc" = 0 ]] && rc=1
  fi
done
echo "$rc" > "$out/teardown.rc"
grep -E '^(TEARDOWN_OBSERVE|ASSERT_TEARDOWN)' "$out/teardown.log" || true
exit "$rc"
