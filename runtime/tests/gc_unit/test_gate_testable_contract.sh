#!/usr/bin/env bash
# Compatibility entry; controlled gate checks are zx ESM.
exec zx "$(dirname "$0")/test_gate_testable_contract.mjs" "$@"
