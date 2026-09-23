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
env -u GC_UNIT_LIST_TESTS -u GC_UNIT_ABORT_BEFORE -u GC_UNIT_TALLY_FILE   GC_UNIT_OTHER_VM_CHILD=RuntimeWorkers.ActivePoolBeforeHarnessShutdown   LD_LIBRARY_PATH="$lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"   timeout 120 gdb -nx -batch -x "$src/check_other_vm_teardown.py"   --args "$elf" --gtest_filter=RuntimeWorkers.ActivePoolBeforeHarnessShutdown   > "$out/teardown.log" 2>&1
rc=$?
set -e
if [[ "$rc" = 0 ]] && ! grep -Fq 'GC_UNIT_OTHER_VM_OKIDOKI RuntimeWorkers.ActivePoolBeforeHarnessShutdown' "$out/teardown.log"; then
  echo 'ASSERT_TEARDOWN_SENTINEL FAIL' >> "$out/teardown.log"
  rc=1
fi
echo "$rc" > "$out/teardown.rc"
grep -E '^(TEARDOWN_OBSERVE|ASSERT_TEARDOWN)' "$out/teardown.log" || true
exit "$rc"
