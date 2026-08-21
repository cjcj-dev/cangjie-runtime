#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 OFFICIAL_SDK OUTPUT_DIR" >&2
    exit 2
fi

sdk=$(realpath "$1")
out=$2
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cjc="$sdk/bin/cjc"
runtime_lib="$sdk/runtime/lib/linux_x86_64_cjnative"

[[ -x "$cjc" ]] || { echo "official cjc is not executable: $cjc" >&2; exit 2; }
[[ -f "$runtime_lib/libcangjie-runtime.so" ]] || {
    echo "official runtime is missing: $runtime_lib/libcangjie-runtime.so" >&2
    exit 2
}
[[ ! -e "$out" ]] || { echo "refusing to overwrite: $out" >&2; exit 2; }
install -d "$out/bin" "$out/logs"

compiler_sha=$(sha256sum "$cjc" | awk '{print $1}')
compiler_stamp=$(strings "$cjc" | grep -oE 'CJCJ-COMMIT:[0-9a-f-]+' | sort -u | paste -sd, -)
[[ -n "$compiler_stamp" ]] || compiler_stamp=NO_PROVENANCE_STAMP

printf '%s\n' \
    $'workload\tsource\tsource_sha256\tcompiler\tcompiler_sha256\tcompiler_stamp\tbinary\tbinary_sha256\tmarker\twork_units\twork_unit_name' \
    >"$out/workloads.tsv"

build_one() {
    local id=$1 source=$2 marker=$3 work_units=$4 work_unit_name=$5
    local binary="$out/bin/$id"
    local log="$out/logs/build-$id.log"

    env -i \
        HOME=/root \
        LC_ALL=C \
        PATH="$sdk/bin:$sdk/tools/bin:$sdk/third_party/llvm/bin:/usr/bin:/bin" \
        CANGJIE_HOME="$sdk" \
        LD_LIBRARY_PATH="$runtime_lib:$sdk/tools/lib:$sdk/third_party/llvm/lib" \
        "$cjc" "$source" -O0 --static-std -o "$binary" >"$log" 2>&1

    [[ -x "$binary" ]] || { echo "build produced no executable: $binary" >&2; exit 1; }
    local source_sha binary_sha
    source_sha=$(sha256sum "$source" | awk '{print $1}')
    binary_sha=$(sha256sum "$binary" | awk '{print $1}')
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$id" "$source" "$source_sha" "$cjc" "$compiler_sha" "$compiler_stamp" \
        "$binary" "$binary_sha" "$marker" "$work_units" "$work_unit_name" \
        >>"$out/workloads.tsv"
}

build_one SD "$script_dir/../gcparity/survival_dense.cj" SURVIVAL_DENSE_OK 15360000 allocation_steps
build_one NW "$script_dir/natural_wave_notime.cj" NATURAL_WAVE_OK 12 completed_waves

sha256sum "$out/workloads.tsv" >"$out/workloads.tsv.sha256"
echo "WORKLOAD_BUILD_OK compiler_sha256=$compiler_sha manifest=$out/workloads.tsv"
