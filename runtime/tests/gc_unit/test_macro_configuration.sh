#!/usr/bin/env bash
# Integration regression: run the real standalone entry with unchanged SOs.
# Use an isolated source tree: transient runner copies are created beside it.
set -euo pipefail
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=${GC_MACRO_TEST_OUT:?set GC_MACRO_TEST_OUT}
DEFAULT_LIB=${GC_MACRO_DEFAULT_LIB:?set GC_MACRO_DEFAULT_LIB}
TESTABLE_LIB=${GC_MACRO_TESTABLE_LIB:?set GC_MACRO_TESTABLE_LIB}
DEFAULT_OUTPUT=${GC_MACRO_DEFAULT_OUTPUT:?set GC_MACRO_DEFAULT_OUTPUT to published headers and recipe}
TESTABLE_OUTPUT=${GC_MACRO_TESTABLE_OUTPUT:?set GC_MACRO_TESTABLE_OUTPUT to published headers and recipe}
mkdir -p "$OUT"
producer=$(mktemp "$SRC/run_standalone.producer.XXXXXX")
consumer=$(mktemp "$SRC/run_standalone.consumer.XXXXXX")
restore_source=0
cleanup() {
  if [[ "$restore_source" == 1 ]]; then
    cp "$OUT/test_store_barrier_buffer.original.cpp" "$SRC/test_store_barrier_buffer.cpp"
  fi
  rm -f "$producer" "$consumer"
}
trap cleanup EXIT
python3 - "$SRC/run_standalone.sh" "$producer" "$consumer" "$OUT" <<'PY'
import difflib
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
for name, path, old, new in (
    ('producer', sys.argv[2], '  TEST_DEFINES+=(-DMRT_GC_UNIT_TESTS=1)', '  : # disconnected macro producer'),
    ('consumer', sys.argv[3], '  "${compiler[@]}" "${flags[@]}" -c "$source" -o "$object"',
     '  "${compiler[@]}" "${flags[@]}" -UMRT_GC_UNIT_TESTS -c "$source" -o "$object"'),
):
    assert source.count(old) == 1, (name, 'bearing point changed')
    cut = source.replace(old, new)
    Path(path).write_text(cut)
    Path(sys.argv[4], name + '.diff').write_text(''.join(difflib.unified_diff(
        source.splitlines(True), cut.splitlines(True),
        fromfile='a/runtime/tests/gc_unit/run_standalone.sh',
        tofile='b/runtime/tests/gc_unit/run_standalone.sh')))
PY
run_arm() {
  local name=$1 script=$2 lib=$3 testable=$4 output=$5
  mkdir -p "$OUT/$name"
  uptime >"$OUT/$name/uptime-before.txt"
  sha256sum "$script" "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" >"$OUT/$name/inputs.sha256"
  local start=$SECONDS rc
  set +e
  env GCV2_RUNTIME_OUTPUT_ROOT="$output" MRT_TESTABLE_INTERNALS="$testable" \
    GC_UNIT_OUT="$OUT/$name" GCV2_RUNTIME_LIB_DIR="$lib" \
    bash "$script" >"$OUT/$name/run.log" 2>&1
  rc=$?
  set -e
  echo "$rc" >"$OUT/$name/run.rc"
  echo "$((SECONDS-start))" >"$OUT/$name/wall.txt"
  uptime >"$OUT/$name/uptime-after.txt"
  sha256sum "$OUT/$name/cj_gc_unit" >"$OUT/$name/elf.sha256"
}
# Distinct output directories; compile and run independent arms concurrently.
run_arm green "$SRC/run_standalone.sh" "$TESTABLE_LIB" 1 "$TESTABLE_OUTPUT" &
run_arm producer "$producer" "$TESTABLE_LIB" 1 "$TESTABLE_OUTPUT" &
run_arm consumer "$consumer" "$TESTABLE_LIB" 1 "$TESTABLE_OUTPUT" &
run_arm default "$SRC/run_standalone.sh" "$DEFAULT_LIB" 0 "$DEFAULT_OUTPUT" &
run_arm default-consumer "$consumer" "$DEFAULT_LIB" 0 "$DEFAULT_OUTPUT" &
run_arm restored "$SRC/run_standalone.sh" "$TESTABLE_LIB" 1 "$TESTABLE_OUTPUT" &
wait
# Positive control of the per-name ruler: disable only the existing StoreBuf
# declaration in this isolated tree, after the concurrent compilers finish.
cp "$SRC/test_store_barrier_buffer.cpp" "$OUT/test_store_barrier_buffer.original.cpp"
restore_source=1
python3 - "$SRC/test_store_barrier_buffer.cpp" <<'PYONE'
from pathlib import Path
import sys
path = Path(sys.argv[1])
source = path.read_text()
old = '#if defined(MRT_GC_UNIT_TESTS) && defined(__linux__)'
assert source.count(old) == 1
path.write_text(source.replace(old, '#if defined(MRT_GC_UNIT_TESTS_MISSPELLED) && defined(__linux__)'))
PYONE
run_arm missing-one "$SRC/run_standalone.sh" "$TESTABLE_LIB" 1 "$TESTABLE_OUTPUT"
cp "$OUT/test_store_barrier_buffer.original.cpp" "$SRC/test_store_barrier_buffer.cpp"
restore_source=0
sha256sum "$OUT/test_store_barrier_buffer.original.cpp" "$SRC/test_store_barrier_buffer.cpp" >"$OUT/source-restored.sha256"
python3 - "$OUT" "$SRC/gc_unit_macro_tests.txt" <<'PY'
from pathlib import Path
import sys
out = Path(sys.argv[1])
expected = {line for line in Path(sys.argv[2]).read_text().splitlines()
            if line and not line.startswith('#')}
def names(arm):
    result = set()
    suite = ''
    for line in (out / arm / 'configuration-registered.raw').read_text().splitlines():
        if line.endswith('.') and not line[0].isspace():
            suite = line[:-1]
        elif line.startswith('  '):
            result.add(suite + '.' + line.strip())
    return result
for arm in ('producer', 'consumer'):
    log = (out / arm / 'run.log').read_text()
    assert (out / arm / 'run.rc').read_text().strip() == '1', arm
    failures = {line.split('missing=', 1)[1] for line in log.splitlines()
                if line.startswith('GC_UNIT_MACRO_REGISTRATION_FAIL missing=')}
    assert failures == expected, (arm, failures)
    assert names('green') - names(arm) == expected, arm
    assert not names(arm) - names('green'), arm
    print(f'ASSERT_MACRO_CUT_REACHED arm={arm} missing={len(failures)} rc=1')
assert names('green') == names('restored')
assert names('default') == names('default-consumer')
for arm in ('green', 'restored'):
    for name in expected:
        # The parallel runner supplies stable indexed filenames; inspect its
        # complete aggregate as the independent execution receipt instead.
        text = (out / arm / 'run.log').read_text()
        assert '[  PASS  ] ' + name in text, (arm, name)
    print(f'ASSERT_MACRO_EXECUTED arm={arm} passed={len(expected)}')
for arm in ('default', 'default-consumer'):
    assert '[  PASS  ] StoreBuf.NullAndPreMarkPreviousAreNormalSkips' in (out / arm / 'run.log').read_text()
print('ASSERT_MACRO_DEFAULT_CONTROL registry_unchanged=1 control_executed=1')
assert (out / 'missing-one' / 'run.rc').read_text().strip() == '1'
assert names('green') - names('missing-one') == {'StoreBuf.YoungSlotExcludedFromOldPhaseSnapshot'}
assert not names('missing-one') - names('green')
print('ASSERT_MACRO_ONE_MISSING_REACHED missing=StoreBuf.YoungSlotExcludedFromOldPhaseSnapshot rc=1')
PY
