#!/usr/bin/env bash
# Source-only deliverable. Execute only after controller's new batch approval.
# One input, one fresh inferior, one 120s attempt. Never rebuild or retry.
set -euo pipefail
ulimit -c 0
if [[ $# != 4 ]]; then
  echo 'usage: a2_qualify_case.sh CASE TEST_ELF PRODUCT_LIB_DIR EVIDENCE_DIR' >&2
  exit 2
fi
case_name=$1
elf=$(realpath "$2")
product=$(realpath "$3")
evidence=$4
script_dir=$(cd "$(dirname "$0")" && pwd)
case "$case_name" in
  prefix-ordinary) test_name=PackageInit.PrefixOrdinaryQualification ;;
  prefix-continuous) test_name=PackageInit.PrefixContinuousQualification ;;
  prefix-invalid) test_name=PackageInit.PrefixInvalidQualification ;;
  roots-missing) test_name=ManagedMetadata.RootsMissingQualification ;;
  roots-zero) test_name=ManagedMetadata.RootsZeroQualification ;;
  *) echo 'INVALID: unknown case' >&2; exit 2 ;;
esac
mkdir -p "$evidence"
# Refuse to overwrite a previous attempt. A resource/observer failure is kept.
if [[ -e "$evidence/$case_name.manifest.json" || -e "$evidence/$case_name.log" ]]; then
  echo 'INVALID: this case already has an attempt' >&2
  exit 2
fi
python3 - "$elf" "$product" "$evidence/$case_name.manifest.json" <<'PY'
import glob, hashlib, json, os, sys
elf, product, out = sys.argv[1:]
paths = [elf] + glob.glob(product + '/*.so') + glob.glob(os.path.dirname(elf) + '/libcj_metadata*.so')
manifest = {}
for path in paths:
    path = os.path.realpath(path)
    hasher = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            hasher.update(block)
    manifest[path] = hasher.hexdigest()
with open(out, 'x') as stream:
    json.dump(manifest, stream, indent=2)
PY
# Use the existing OTHER_VM child contract directly in this newly exec'd
# debugger inferior, so the observed consumer is not hidden in another exec.
child=()
if [[ $case_name == prefix-* ]]; then
  child=("GC_UNIT_OTHER_VM_CHILD=$test_name")
fi
uptime > "$evidence/$case_name.uptime-before"
start_seconds=$SECONDS
set +e
env "${child[@]}" LD_LIBRARY_PATH="$product${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  A2_CASE="$case_name" A2_INPUT_MANIFEST="$evidence/$case_name.manifest.json" \
  A2_OBSERVER_OUT="$evidence/$case_name.json" \
  timeout 120s gdb -q -nx -batch -x "$script_dir/a2_read_boundaries_gdb.py" \
    --args "$elf" "--gtest_filter=$test_name" > "$evidence/$case_name.log" 2>&1
rc=$?
set -e
printf '%s\n' "$rc" > "$evidence/$case_name.rc"
printf 'wall=%ss\n' "$((SECONDS - start_seconds))" > "$evidence/$case_name.wall"
uptime > "$evidence/$case_name.uptime-after"
if [[ ! -f "$evidence/$case_name.json" ]]; then
  printf '{"case":"%s","status":"INVALID","reason":"observer did not produce a record","wrapper_rc":%s}\n' \
    "$case_name" "$rc" > "$evidence/$case_name.json"
fi
exit "$rc"
