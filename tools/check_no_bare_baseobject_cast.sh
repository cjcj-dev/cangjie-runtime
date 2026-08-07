#!/usr/bin/env bash
# Fail if production sources contain bare reinterpret_cast<BaseObject*>.
# Sole allowed site: runtime/src/Common/ColourTypes.h (to_object and friends).
# Design: ops/design/COLOUR_TYPE_DISCIPLINE.md + ctyperest.
# ⚠ Uses grep (not rg): kkk2 measurement boxes often lack ripgrep.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
hits="$(
  grep -RnE --include='*.cpp' --include='*.h' --include='*.hpp' --include='*.cc' --include='*.inc' \
    'reinterpret_cast[[:space:]]*<[[:space:]]*BaseObject[[:space:]]*\*>' runtime/src 2>/dev/null \
    | grep -v '/tests/' \
    | grep -v 'ColourTypes\.h:' \
    || true
)"
if [[ -n "$hits" ]]; then
  echo "FAIL: bare reinterpret_cast<BaseObject*> outside ColourTypes.h:" >&2
  printf '%s\n' "$hits" >&2
  exit 1
fi
echo "OK: no bare reinterpret_cast<BaseObject*> outside ColourTypes.h"
