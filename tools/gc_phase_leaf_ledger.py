#!/usr/bin/env python3
"""Thin CLI for the common GCLOG v3 structural-leaf ledger."""

from __future__ import annotations

import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime/tests/perf_vs_official"))

from gclog_schema import phase_leaf_ledger  # noqa: E402


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} LOG", file=sys.stderr)
        return 2
    try:
        result = phase_leaf_ledger(Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace"))
    except ValueError as exc:
        print(f"PHASE_LEAF_LEDGER_FAIL: {exc}", file=sys.stderr)
        return 1
    print("PHASE_LEAF_LEDGER_OK " + json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
