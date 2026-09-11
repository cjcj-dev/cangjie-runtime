#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
resolver="$ROOT/runtime/build/resolve_runtime_output.sh"
fixture=$(mktemp -d /tmp/runtime-output-resolver.XXXXXX)
trap 'rm -rf "$fixture"' EXIT

make_config() {
    local id=$1 signature=$2
    local lib="$fixture/output/temp/$id/lib/x86_64_Release"
    mkdir -p "$lib"
    printf 'runtime-%s\n' "$id" >"$lib/libcangjie-runtime.so"
    printf 'bounds-%s\n' "$id" >"$lib/libboundscheck.so"
    printf '%s\n' \
        'SCHEMA_VERSION=1' \
        "CONFIG_ID=$id" \
        "CONFIG_SIGNATURE_SHA256=$signature" \
        "RUNTIME_SHA256=$(sha256sum "$lib/libcangjie-runtime.so" | awk '{print $1}')" \
        "BOUNDSCHECK_SHA256=$(sha256sum "$lib/libboundscheck.so" | awk '{print $1}')" \
        "LIB_DIR=$lib" >"$fixture/output/temp/$id/runtime-build-config.txt"
}

make_config linux-x86_64-release-default-111111111111 \
    1111111111111111111111111111111111111111111111111111111111111111
make_config linux-x86_64-release-gcunit-222222222222 \
    2222222222222222222222222222222222222222222222222222222222222222

selected=$(bash "$resolver" "$fixture" linux-x86_64-release-gcunit-222222222222)
[[ "$selected" == "$fixture/output/temp/linux-x86_64-release-gcunit-222222222222/lib/x86_64_Release" ]]
runtime_sha=$(sha256sum "$selected/libcangjie-runtime.so" | awk '{print $1}')
bounds_sha=$(sha256sum "$selected/libboundscheck.so" | awk '{print $1}')
printf 'RUNTIME_OUTPUT_RESOLVER_OK config=%s runtime_sha256=%s boundscheck_sha256=%s\n' \
    linux-x86_64-release-gcunit-222222222222 "$runtime_sha" "$bounds_sha"

# A directory name alone is not identity: a mismatching manifest must fail.
/usr/bin/sed -i 's/^CONFIG_ID=.*/CONFIG_ID=wrong/' \
    "$fixture/output/temp/linux-x86_64-release-gcunit-222222222222/runtime-build-config.txt"
set +e
bash "$resolver" "$fixture" linux-x86_64-release-gcunit-222222222222 \
    >"$fixture/mismatch.log" 2>&1
mismatch_rc=$?
set -e
if [[ $mismatch_rc -ne 3 ]]; then
    printf 'RUNTIME_OUTPUT_RESOLVER_NEGATIVE_FAIL expected_rc=3 actual_rc=%s\n' \
        "$mismatch_rc" >&2
    exit 1
fi
/usr/bin/grep -q "manifest says 'wrong'" "$fixture/mismatch.log"
printf 'RUNTIME_OUTPUT_RESOLVER_NEGATIVE_OK rc=%s\n' "$mismatch_rc"

# Restore the valid ID, then change one real selected file: the hash check must
# reject it while the other configured publication still resolves normally.
/usr/bin/sed -i 's/^CONFIG_ID=.*/CONFIG_ID=linux-x86_64-release-gcunit-222222222222/' \
    "$fixture/output/temp/linux-x86_64-release-gcunit-222222222222/runtime-build-config.txt"
printf 'changed\n' >>"$selected/libcangjie-runtime.so"
set +e
bash "$resolver" "$fixture" linux-x86_64-release-gcunit-222222222222 >"$fixture/hash.log" 2>&1
hash_rc=$?
set -e
[[ $hash_rc -eq 5 ]]
bash "$resolver" "$fixture" linux-x86_64-release-default-111111111111 >/dev/null
printf 'RUNTIME_OUTPUT_RESOLVER_HASH_NEGATIVE_OK rc=%s control_rc=0\n' "$hash_rc"
