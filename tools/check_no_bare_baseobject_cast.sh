#!/usr/bin/env bash
# Fail if production sources contain bare reinterpret_cast<BaseObject*>.
# Sole allowed site: runtime/src/Common/ColourTypes.h (to_object and friends).
# Design: ops/design/COLOUR_TYPE_DISCIPLINE.md + ctyperest.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mapfile -t hits < <(rg -n 'reinterpret_cast\s*<\s*BaseObject\s*\*>' \
  -g '*.cpp' -g '*.h' -g '*.hpp' -g '*.cc' -g '*.inc' \
  --glob '!runtime/tests/**' \
  --glob '!**/ColourTypes.h' \
  runtime/src 2>/dev/null || true)
if ((${#hits[@]} > 0)); then
  echo "FAIL: bare reinterpret_cast<BaseObject*> outside ColourTypes.h:" >&2
  printf '%s\n' "${hits[@]}" >&2
  exit 1
fi
echo "OK: no bare reinterpret_cast<BaseObject*> outside ColourTypes.h"
