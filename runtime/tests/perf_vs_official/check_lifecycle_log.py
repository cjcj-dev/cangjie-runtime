#!/usr/bin/env python3
"""A6: consume a single real GcLifecycleLog.CollectionEnd process ledger."""
import sys
from pathlib import Path
from analyze_stw import cycle_durs
from gclog_schema import parse_gclog

text = Path(sys.argv[1]).read_text()
records = parse_gclog(text).validate_complete()
durations, kinds = cycle_durs(text)
minor = sum(k == 'minor' for k in kinds.values())
major = sum(k == 'major' for k in kinds.values())
print(f'A6_TARGET cycles={len(durations)} minor={minor} major={major} aborted={records.aborted} truncated={records.truncated}', flush=True)
assert minor > 0 and major > 0
assert records.truncated == 0
