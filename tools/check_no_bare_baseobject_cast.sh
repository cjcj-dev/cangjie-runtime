#!/usr/bin/env bash
# Fail if production sources contain an unreviewed cast to a managed object or
# slot spelling. Exact existing representation boundaries live in the adjacent
# allowlist with one source line and one reason per exemption.
# Design: ops/design/COLOUR_TYPE_DISCIPLINE.md + ctyperest.
# ⚠ Uses grep (not rg): kkk2 measurement boxes often lack ripgrep.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
ALLOWLIST="tools/check_no_bare_baseobject_cast.allowlist"
SELF_TEST=false
case "${1:-}" in
  "") ;;
  --self-test) SELF_TEST=true ;;
  *) echo "usage: $0 [--self-test]" >&2; exit 2 ;;
esac

if [[ ! -r "$ALLOWLIST" ]]; then
  echo "FAIL: cast allowlist is missing or unreadable: $ALLOWLIST" >&2
  exit 1
fi

declare -A allowed=()
declare -a allowed_order=()
while IFS=$'\t' read -r key reason extra; do
  [[ -z "$key" || "$key" == \#* ]] && continue
  if [[ ! "$key" =~ ^runtime/src/.+:[0-9]+$ || -z "$reason" || -n "$extra" ]]; then
    echo "FAIL: malformed cast exemption (expected path:line<TAB>reason): $key" >&2
    exit 1
  fi
  if [[ -n "${allowed[$key]+present}" ]]; then
    echo "FAIL: duplicate cast exemption: $key" >&2
    exit 1
  fi
  allowed["$key"]="$reason"
  allowed_order+=("$key")
done < "$ALLOWLIST"

if (( ${#allowed_order[@]} == 0 )); then
  echo "FAIL: cast allowlist contains no exemptions" >&2
  exit 1
fi

hits="$(
  grep -RnE --include='*.cpp' --include='*.h' --include='*.hpp' --include='*.cc' --include='*.inc' \
    '(reinterpret_cast|static_cast)[[:space:]]*<[[:space:]]*(const[[:space:]]+)?(BaseObject|RefField|HeapSlot|RootSlot|DerivedSlot|ObjectRef|MArray|MObject)([[:space:]]*<[^>]*>)?[[:space:]]*[\*&]' \
    runtime/src 2>/dev/null \
    | grep -v '/tests/' \
    | grep -vE ':[[:space:]]*(//|/\*)' \
    || true
)"

declare -A seen=()
unexpected=""
while IFS= read -r hit; do
  [[ -z "$hit" ]] && continue
  path="${hit%%:*}"
  rest="${hit#*:}"
  line="${rest%%:*}"
  key="$path:$line"
  seen["$key"]=1
  if [[ -z "${allowed[$key]+present}" ]]; then
    unexpected+="${unexpected:+$'\n'}$hit"
  fi
done <<< "$hits"

stale=""
for key in "${allowed_order[@]}"; do
  if [[ -z "${seen[$key]+present}" ]]; then
    stale+="${stale:+$'\n'}$key"
  fi
done

if [[ -n "$unexpected" ]]; then
  echo "FAIL: guarded casts lack exact exemptions:" >&2
  printf '%s\n' "$unexpected" >&2
  exit 1
fi
if [[ -n "$stale" ]]; then
  echo "FAIL: stale cast exemptions no longer identify guarded source lines:" >&2
  printf '%s\n' "$stale" >&2
  exit 1
fi

echo "OK: guarded_casts=${#seen[@]} exact_exemptions=${#allowed[@]}"

if [[ "$SELF_TEST" == true ]]; then
  removed="${allowed_order[0]}"
  saved_reason="${allowed[$removed]}"
  unset "allowed[$removed]"
  exposed=0
  while IFS= read -r hit; do
    [[ -z "$hit" ]] && continue
    path="${hit%%:*}"
    rest="${hit#*:}"
    line="${rest%%:*}"
    key="$path:$line"
    if [[ "$key" == "$removed" && -z "${allowed[$key]+present}" ]]; then
      exposed=$((exposed + 1))
    fi
  done <<< "$hits"
  allowed["$removed"]="$saved_reason"
  if (( exposed == 0 )); then
    echo "FAIL: removing exemption did not expose its guarded cast: $removed" >&2
    exit 1
  fi
  echo "SELFTEST-PASS: removed_exemption=$removed exposed_hits=$exposed"
fi
