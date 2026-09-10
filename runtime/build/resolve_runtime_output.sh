#!/usr/bin/env bash
# Resolve one explicitly named runtime configuration and verify its manifest.
set -euo pipefail

runtime_root=${1:?usage: resolve_runtime_output.sh RUNTIME_ROOT CONFIG_ID}
config_id=${2:?usage: resolve_runtime_output.sh RUNTIME_ROOT CONFIG_ID}

if [[ ! "$config_id" =~ ^[a-z0-9._+-]+$ ]]; then
    echo "RUNTIME_OUTPUT_RESOLVE_FAIL: invalid configuration id '$config_id'" >&2
    exit 2
fi

config_root="$runtime_root/output/temp/$config_id"
manifest="$config_root/runtime-build-config.txt"
if [[ ! -f "$manifest" ]]; then
    echo "RUNTIME_OUTPUT_RESOLVE_FAIL: missing manifest for '$config_id'" >&2
    exit 2
fi

manifest_id=$(/usr/bin/sed -n 's/^CONFIG_ID=//p' "$manifest")
lib_dir=$(/usr/bin/sed -n 's/^LIB_DIR=//p' "$manifest")
signature=$(/usr/bin/sed -n 's/^CONFIG_SIGNATURE_SHA256=//p' "$manifest")
if [[ "$manifest_id" != "$config_id" ]]; then
    echo "RUNTIME_OUTPUT_RESOLVE_FAIL: requested '$config_id', manifest says '$manifest_id'" >&2
    exit 3
fi
if [[ ! "$signature" =~ ^[0-9a-f]{64}$ ]]; then
    echo "RUNTIME_OUTPUT_RESOLVE_FAIL: invalid configuration signature" >&2
    exit 3
fi

config_root_real=$(realpath "$config_root")
lib_dir_real=$(realpath -m "$lib_dir")
case "$lib_dir_real/" in
    "$config_root_real"/*) ;;
    *)
        echo "RUNTIME_OUTPUT_RESOLVE_FAIL: library directory escapes configuration root" >&2
        exit 3
        ;;
esac
if [[ ! -f "$lib_dir_real/libcangjie-runtime.so" ]]; then
    echo "RUNTIME_OUTPUT_RESOLVE_FAIL: missing libcangjie-runtime.so for '$config_id'" >&2
    exit 4
fi
if [[ ! -f "$lib_dir_real/libboundscheck.so" ]]; then
    echo "RUNTIME_OUTPUT_RESOLVE_FAIL: missing libboundscheck.so for '$config_id'" >&2
    exit 4
fi

# The manifest binds the selected path to the published bytes, not just a name.
for pair in RUNTIME:libcangjie-runtime.so BOUNDSCHECK:libboundscheck.so; do
    field=${pair%%:*}
    file=${pair#*:}
    expected=$(/usr/bin/sed -n "s/^${field}_SHA256=//p" "$manifest")
    actual=$(sha256sum "$lib_dir_real/$file")
    actual=${actual%% *}
    if [[ ! "$expected" =~ ^[0-9a-f]{64}$ || "$actual" != "$expected" ]]; then
        echo "RUNTIME_OUTPUT_RESOLVE_FAIL: $file hash differs from published manifest" >&2
        exit 5
    fi
done

printf '%s\n' "$lib_dir_real"
