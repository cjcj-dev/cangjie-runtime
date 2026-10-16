#!/usr/bin/env bash

gc_unit_language_admit() {
  local admission
  admission="$(python3 "$(dirname "${BASH_SOURCE[0]}")/language_toolchain.py")" || return "$?"
  GC_UNIT_LANGUAGE_IDENTITY="$admission"
  CJC_BIN="$GC_UNIT_LANGUAGE_SDK/bin/cjc"
  read -r CJC_SHA256 LLC_SHA256 OPT_SHA256 STD_SHA256 STD_CORE_SHA256 CJC_RUNTIME_SHA256 < <(
    python3 -c 'import json,sys; record=json.loads(sys.argv[1]); print(*(record["language"][key] for key in ("cjc", "llc", "opt", "std", "std_core")), record["compiler_host"])' "$admission"
  )
}

gc_unit_language_environment() {
  export CANGJIE_HOME="$GC_UNIT_LANGUAGE_SDK"
  export CJC="$CJC_BIN"
}

gc_unit_language_compile() {
  local compiler_rc=0
  "$CJC_BIN" "$@" || compiler_rc=$?
  printf '%s\n' "$compiler_rc" >>"${GC_UNIT_OUT:?}/compile.rc"
  return "$compiler_rc"
}
