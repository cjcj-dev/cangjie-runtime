#!/usr/bin/env bash
# Regression for old_tail's initialization boundary, using the real fixture.
set -euo pipefail
ulimit -c 0
: "${GC_UNIT_TEST_ELF:?}" "${GCV2_RUNTIME_LIB_DIR:?}" "${DIRECTOR_SOURCE:?}"
: "${TAIL_OUT:?}" "${TAIL_CPUSET:?}"
script_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$TAIL_OUT"
export LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export TAIL_FIXTURE_RESULT="$TAIL_OUT/fixture-result.json"
rm -f "$TAIL_FIXTURE_RESULT"
timeout 45 taskset -c "$TAIL_CPUSET" gdb -nx -batch \
    -ex "source $script_dir/test_old_tail_fixture_gdb.py" "$GC_UNIT_TEST_ELF"
# GDB can exit from a breakpoint callback before flushing its Python output.
# Require the actual API argument observation, not merely a zero debugger rc.
python3 - "$TAIL_FIXTURE_RESULT" <<'PY'
import json
import sys
from pathlib import Path

result = json.loads(Path(sys.argv[1]).read_text())
print('ASSERT_FIXTURE_PARAMETERS ' + json.dumps(result, sort_keys=True), flush=True)
assert result['passed'] and result['actual'] == result['expected'], result
PY
