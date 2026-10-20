#!/usr/bin/env bash
# Compatibility entry; orchestration is zx ESM.
exec zx "$(dirname "$0")/gate_gc_unit.mjs" "$@"
